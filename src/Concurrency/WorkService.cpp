//
// Created by Geenz on 7/7/25.
//

#include "WorkService.h"
#include <cmath>
#include <algorithm>
#include <limits>

#include "WorkContractGroup.h"
#include "WorkGraph.h"
#include "AdaptiveRankingScheduler.h"

namespace EntropyEngine {
namespace Core {
namespace Concurrency {
    thread_local size_t WorkService::stSoftFailureCount = 0;
    thread_local size_t WorkService::stThreadId = 0;

    WorkService::WorkService(Config config, std::unique_ptr<IWorkScheduler> scheduler)
        : _config(config) {

        // Always clamp to a range of 1 to hardware concurrency.
        if (_config.threadCount == 0) {
            _config.threadCount = std::thread::hardware_concurrency();
        }
        _config.threadCount = std::clamp(_config.threadCount, (uint32_t)1, std::thread::hardware_concurrency());

        // Update scheduler config with thread count
        _config.schedulerConfig.threadCount = _config.threadCount;

        // Create scheduler if not provided
        if (!scheduler) {
            _scheduler = std::make_unique<AdaptiveRankingScheduler>(_config.schedulerConfig);
        } else {
            _scheduler = std::move(scheduler);
        }
    }

    WorkService::~WorkService() {
        stop();
        clear();
    }

    void WorkService::start() {
        if (_running) {
            return; // Already running
        }

        for (uint32_t i = 0; i < _config.threadCount; i++) {
            _threads.emplace_back([this, threadId = i](const std::stop_token& stoken) {
                stThreadId = threadId;
                executeWork(stoken);
            });
        }

        _running = true;
    }

    void WorkService::requestStop() {
        for (auto &thread : _threads) {
            thread.request_stop();
        }

        // Wake up any threads waiting on the condition variable
        _workAvailable = true;
        _workAvailableCV.notify_all();
    }

    void WorkService::waitForStop() {
        for (auto &thread : _threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        _threads.clear();
        _running = false;

        // Reset thread-local state after all threads have stopped
        resetThreadLocalState();
    }

    void WorkService::stop() {
        requestStop();
        waitForStop();
    }

    bool WorkService::isRunning() const {
        return _running;
    }

    void WorkService::clear() {
        std::unique_lock<std::shared_mutex> lock(_workContractGroupsMutex);

        // Release and disconnect all groups we retained on add
        for (auto* group : _workContractGroups) {
            if (group) {
                // Clear back-reference so the group won't call back into us during destruction
                group->setConcurrencyProvider(nullptr);
                // Release the retain acquired in addWorkContractGroup
                group->release();
            }
        }

        _workContractGroups.clear();
        _workContractGroupCount = 0;

        // Notify scheduler
        _scheduler->notifyGroupsChanged({});
        _scheduler->reset();
    }

    WorkService::GroupOperationStatus WorkService::addWorkContractGroup(WorkContractGroup* contractGroup) {
        // This is generally MUCH simpler than the old atomic stuff.
        // Old atomic tracking of the contract group had a bunch of epoch-based tracking that was very complex.
        // Also required reclamation of vectors that honestly just isn't worth it on a cold path like this.
        std::unique_lock<std::shared_mutex> lock(_workContractGroupsMutex);

        // Check for existence to prevent duplicates
        if (std::find(_workContractGroups.begin(), _workContractGroups.end(), contractGroup) != _workContractGroups.end()) {
            return GroupOperationStatus::Exists;
        }

        // Add the group
        _workContractGroups.push_back(contractGroup);
        _workContractGroupCount++;

        // Retain the group while registered with the service (ref-counted semantics)
        if (contractGroup) {
            contractGroup->retain();
        }

        // Notify scheduler of group change
        _scheduler->notifyGroupsChanged(_workContractGroups);

        // Set ourselves as the concurrency provider for this group
        contractGroup->setConcurrencyProvider(this);

        return GroupOperationStatus::Added;
    }

    WorkService::GroupOperationStatus WorkService::removeWorkContractGroup(WorkContractGroup* contractGroup) {
        std::unique_lock<std::shared_mutex> lock(_workContractGroupsMutex);

        auto it = std::find(_workContractGroups.begin(), _workContractGroups.end(), contractGroup);
        if (it == _workContractGroups.end()) {
            return GroupOperationStatus::NotFound;
        }

        // Remove the group
        _workContractGroups.erase(it);
        _workContractGroupCount--;

        // Notify scheduler of group change
        _scheduler->notifyGroupsChanged(_workContractGroups);

        // Clear the concurrency provider for this group
        contractGroup->setConcurrencyProvider(nullptr);

        // Release the retain taken during add
        contractGroup->release();

        return GroupOperationStatus::Removed;
    }

    size_t WorkService::getWorkContractGroupCount() const {
        std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);
        return _workContractGroupCount;
    }

    size_t WorkService::getThreadCount() const {
        return _config.threadCount;
    }

    size_t WorkService::getSoftFailureCount() const {
        return _config.maxSoftFailureCount;
    }

    size_t WorkService::setSoftFailureCount(size_t softFailureCount) {
        if (softFailureCount != _config.maxSoftFailureCount) {
            _config.maxSoftFailureCount = softFailureCount;
        }
        return _config.maxSoftFailureCount;
    }

    size_t WorkService::getFailureSleepTime() const {
        return _config.failureSleepTime;
    }

    size_t WorkService::setFailureSleepTime(size_t failureSleepTime) {
        if (failureSleepTime != _config.failureSleepTime) {
            _config.failureSleepTime = failureSleepTime;
        }

        return _config.failureSleepTime;
    }

    void WorkService::executeWork(const std::stop_token& token) {
        WorkContractGroup* lastExecutedGroup = nullptr;

        while (!token.stop_requested()) {
            // Get current snapshot of groups with shared_lock (HOT PATH)
            std::vector<WorkContractGroup*> groupsSnapshot;
            {
                std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);
                groupsSnapshot = _workContractGroups;
            }

            if (groupsSnapshot.empty()) {
                // Check for ready timers before sleeping
                checkTimedDeferrals();

                // Wait on condition variable instead of sleeping
                std::unique_lock<std::mutex> lock(_workAvailableMutex);
                _workAvailableCV.wait_for(lock, std::chrono::milliseconds(1), [this, &token]() {
                    return _workAvailable.load() || token.stop_requested();
                });
                continue;
            }

            // Create scheduling context
            IWorkScheduler::SchedulingContext context{
                stThreadId,
                stSoftFailureCount,
                lastExecutedGroup
            };

            // Ask scheduler for next group
            auto scheduleResult = _scheduler->selectNextGroup(groupsSnapshot, context);

            if (scheduleResult.group) {
                // Skip stopped/paused groups
                if (scheduleResult.group->isStopping()) {
                    // Group is paused, try another one
                    stSoftFailureCount++;
                    continue;
                }

                // Try to get work from the selected group
                // Double-check the group isn't stopping right before we use it
                if (scheduleResult.group->isStopping()) {
                    stSoftFailureCount++;
                    continue;
                }

                auto contract = scheduleResult.group->selectForExecution();
                if (contract.valid()) {
                    // Check stop token again before executing work to prevent deadlocks during shutdown
                    if (token.stop_requested()) {
                        // Abort without executing: transition Executing -> Free safely during shutdown
                        scheduleResult.group->abortExecution(contract);
                        break;
                    }

                    // Execute the work (includes all cleanup)
                    scheduleResult.group->executeContract(contract);

                    // Notify scheduler of successful execution
                    _scheduler->notifyWorkExecuted(scheduleResult.group, stThreadId);

                    // Update tracking
                    lastExecutedGroup = scheduleResult.group;
                    stSoftFailureCount = 0;
                    continue;
                }
            }

            // No work found
            if (scheduleResult.shouldSleep || stSoftFailureCount >= _config.maxSoftFailureCount) {
                // Check for ready timers before sleeping
                checkTimedDeferrals();

                // Use condition variable for efficient waiting
                std::unique_lock<std::mutex> lock(_workAvailableMutex);
                _workAvailable = false;
                _workAvailableCV.wait_for(lock, std::chrono::milliseconds(10), [this, &token]() {
                    return _workAvailable.load() || token.stop_requested();
                });
                stSoftFailureCount = 0;
            } else {
                stSoftFailureCount++;
                std::this_thread::yield();
            }
        }
    }

    void WorkService::checkTimedDeferrals() {
        // Check all work contract groups for ready timed deferrals
        // WorkGraph overrides checkTimedDeferrals() to check its timer queue,
        // while base WorkContractGroup returns 0 (no timers)
        size_t totalScheduled = 0;
        {
            std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);
            for (auto* group : _workContractGroups) {
                totalScheduled += group->checkTimedDeferrals();
            }
        }

        // If any timers were scheduled, wake up waiting worker threads
        if (totalScheduled > 0) {
            notifyWorkAvailable();
        }
    }

    void WorkService::notifyWorkAvailable(WorkContractGroup* group) {
        // We don't need to track which group has work, just that work is available
        _workAvailable = true;
        _workAvailableCV.notify_one();
    }

    void WorkService::notifyGroupDestroyed(WorkContractGroup* group) {
        // When a group is destroyed, remove it without touching refcount to avoid
        // releasing during its destructor.
        std::unique_lock<std::shared_mutex> lock(_workContractGroupsMutex);

        auto it = std::find(_workContractGroups.begin(), _workContractGroups.end(), group);
        if (it != _workContractGroups.end()) {
            _workContractGroups.erase(it);
            _workContractGroupCount--;

            // Notify scheduler of group change
            _scheduler->notifyGroupsChanged(_workContractGroups);

            // Clear the concurrency provider for this group (no release here)
            group->setConcurrencyProvider(nullptr);
        }
    }

    void WorkService::resetThreadLocalState() {
        // This only resets the thread-local state in the calling thread,
        // not in the worker threads. The worker threads reset their own
        // state when they exit in the lambda function in start().
        stSoftFailureCount = 0;
        stThreadId = 0;
    }
    
    WorkService::MainThreadWorkResult WorkService::executeMainThreadWork(size_t maxContracts) {
        MainThreadWorkResult result{0, 0, false};

        // Get current snapshot of groups
        std::vector<WorkContractGroup*> groups;
        {
            std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);
            groups = _workContractGroups;
        }

        size_t remaining = maxContracts;

        // Execute work from each group that has main thread work
        for (auto* group : groups) {
            if (group && group->hasMainThreadWork()) {
                result.groupsWithWork++;
                size_t executed = group->executeMainThreadWork(remaining);
                result.contractsExecuted += executed;
                remaining -= executed;
                
                // Stop if we've hit our limit
                if (remaining == 0) {
                    result.moreWorkAvailable = true;
                    break;
                }
            }
        }
        
        // Check if there's more work available
        if (remaining > 0) {
            for (auto* group : groups) {
                if (group && group->hasMainThreadWork()) {
                    result.moreWorkAvailable = true;
                    break;
                }
            }
        }
        
        return result;
    }
    
    size_t WorkService::executeMainThreadWork(WorkContractGroup* group, size_t maxContracts) {
        if (!group) {
            return 0;
        }
        
        return group->executeMainThreadWork(maxContracts);
    }
    
    bool WorkService::hasMainThreadWork() const {
        std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);

        for (auto* group : _workContractGroups) {
            if (group && group->hasMainThreadWork()) {
                return true;
            }
        }
        
        return false;
    }

} // Concurrency
} // Core
} // EntropyEngine