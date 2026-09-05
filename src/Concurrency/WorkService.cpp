//
// Created by Geenz on 7/7/25.
//

#include "WorkService.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <tracy/Tracy.hpp>  // main-thread work-queue profiling (the per-frame "gap")

#include "../Debug/CpuZoneProfiler.h"  // headless `cpu.zones` mirror
#include "AdaptiveRankingScheduler.h"
#include "WorkContractGroup.h"
#include "WorkGraph.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{
thread_local size_t WorkService::stSoftFailureCount = 0;
thread_local size_t WorkService::stThreadId = 0;
thread_local WorkService* WorkService::stOwner = nullptr;

WorkService::WorkService(Config config, std::unique_ptr<IWorkScheduler> scheduler) : _config(config) {
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

    // Sized here, not in start(): notifiers index it without a lock, so it must exist
    // and never move for the service's whole lifetime.
    _laneWake = std::vector<LaneWake>(_config.threadCount);
    _parkedMaskCoversPool = _config.threadCount <= 64;
}

WorkService::~WorkService() {
    stop();
    clear();
}

void WorkService::start() {
    // exchange, not check-then-set: two concurrent start() calls would both see
    // false and spawn a double set of worker threads.
    if (_running.exchange(true)) {
        return;  // Already running
    }

    for (uint32_t i = 0; i < _config.threadCount; i++) {
        _threads.emplace_back([this, threadId = i](const std::stop_token& stoken) {
            stThreadId = threadId;
            stOwner = this;
            executeWork(stoken, _laneWake[threadId]);
            stOwner = nullptr;
        });
    }
}

void WorkService::requestStop() {
    for (auto& thread : _threads) {
        thread.request_stop();
    }

    // The empty critical section cannot complete while a worker holds the mutex on its
    // way into wait_for, so the notify below cannot precede that worker's registration.
    _workAvailable = true;
    { std::lock_guard<std::mutex> handshake(_workAvailableMutex); }
    _workAvailableCV.notify_all();
}

void WorkService::waitForStop() {
    for (auto& thread : _threads) {
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
    // First, stop the group to prevent new work selection
    // Workers will skip this group via isStopping() checks
    contractGroup->stop();

    {
        std::unique_lock<std::shared_mutex> lock(_workContractGroupsMutex);

        auto it = std::find(_workContractGroups.begin(), _workContractGroups.end(), contractGroup);
        if (it == _workContractGroups.end()) {
            return GroupOperationStatus::NotFound;
        }

        // Remove the group from the list
        _workContractGroups.erase(it);
        _workContractGroupCount--;

        // Notify scheduler of group change
        _scheduler->notifyGroupsChanged(_workContractGroups);

        // Clear the concurrency provider for this group
        contractGroup->setConcurrencyProvider(nullptr);
    }
    // Lock released here

    // Wait for any in-flight contract executions to complete
    // This ensures no worker is actively using the group
    contractGroup->wait();

    // Now safe to release our reference
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

void WorkService::executeWork(const std::stop_token& token, LaneWake& wake) {
    WorkContractGroup* lastExecutedGroup = nullptr;

    while (!token.stop_requested()) {
        WorkContractGroup* selectedGroup = nullptr;
        WorkContractHandle contract;

        // Read BEFORE the poll below: see parkUntilWork().
        const uint64_t wakeSnapshot = wake.seq.load(std::memory_order_acquire);

        // Hold the shared_lock across BOTH the scheduler pick AND the claim
        // (selectForExecution). The claim registers this thread in the group's
        // own counters (selecting, then executing) before the lock is dropped,
        // so from the instant the pointer can outlive this scope the group's
        // destruction protocol (stop + wait on those counters) can see us.
        // Claiming after releasing the lock reopens the window where a worker
        // holds a raw group pointer protected by nothing, and
        // removeWorkContractGroup()/~WorkContractGroup can free the group under
        // it. The unique_lock side (add/remove/notifyGroupDestroyed) is the
        // quiescence barrier that makes this sound.
        {
            std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);

            if (!_workContractGroups.empty()) {
                // Ask scheduler for next group - reads directly from _workContractGroups
                auto scheduleResult = _scheduler->selectNextGroup(_workContractGroups);

                // Select group if valid and not stopping
                if (scheduleResult.group && !scheduleResult.group->isStopping()) {
                    selectedGroup = scheduleResult.group;
                    // Drain this worker's pinned lane first (lane == worker id),
                    // then the shared queue. hasPinnedWork is a single atomic
                    // read, so unpinned workloads pay nothing measurable.
                    if (selectedGroup->hasPinnedWork(stThreadId)) {
                        contract = selectedGroup->selectForPinnedExecution(stThreadId);
                    }
                    if (!contract.valid()) {
                        contract = selectedGroup->selectForExecution();
                    }
                }
            }
        }
        // Shared lock released here; a claimed contract keeps the group alive
        // via its executing count until executeContract()'s final decrement.

        if (!selectedGroup) {
            // No work found - check for ready timers before sleeping
            checkTimedDeferrals();

            // Use condition variable for efficient waiting (100us timeout as safety valve)
            parkUntilWork(token, wake, wakeSnapshot, std::chrono::microseconds(100));
            continue;
        }

        if (contract.valid()) {
            // Check stop token again before executing work to prevent deadlocks during shutdown
            if (token.stop_requested()) {
                // Abort without executing: transition Executing -> Free safely during shutdown
                selectedGroup->abortExecution(contract);
                break;
            }

            // Execute the work (includes all cleanup)
            selectedGroup->executeContract(contract);

            // Notify scheduler of successful execution
            _scheduler->notifyWorkExecuted(selectedGroup, stThreadId);

            // Update tracking
            lastExecutedGroup = selectedGroup;
            stSoftFailureCount = 0;
        } else {
            stSoftFailureCount++;
            if (stSoftFailureCount >= _config.maxSoftFailureCount) {
                // Check for ready timers before sleeping
                checkTimedDeferrals();

                // Use condition variable for efficient waiting (1ms timeout as safety valve)
                parkUntilWork(token, wake, wakeSnapshot, std::chrono::milliseconds(1));
            } else {
                std::this_thread::yield();
            }
        }
    }
}

void WorkService::parkUntilWork(const std::stop_token& token, LaneWake& wake, uint64_t wakeSnapshot,
                                std::chrono::nanoseconds timeout) {
    const uint64_t laneBit = stThreadId < 64 ? (uint64_t(1) << stThreadId) : 0;
    wake.parked.store(1, std::memory_order_seq_cst);
    _parkedMask.fetch_or(laneBit, std::memory_order_seq_cst);
    {
        std::unique_lock<std::mutex> lock(_workAvailableMutex);
        // Consumed, not cleared, so a notify since the failed poll survives the wait;
        // matches the writer's seq_cst. H.1's mask skip and H.3's early-out depend on this.
        _workAvailableCV.wait_for(lock, timeout, [this, &wake, wakeSnapshot, &token]() {
            return (_workAvailable.load(std::memory_order_seq_cst) &&
                    _workAvailable.exchange(false, std::memory_order_acq_rel)) ||
                   wake.seq.load(std::memory_order_seq_cst) != wakeSnapshot || token.stop_requested();
        });
    }
    _parkedMask.fetch_and(~laneBit, std::memory_order_seq_cst);
    wake.parked.store(0, std::memory_order_release);
    stSoftFailureCount = 0;
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

void WorkService::notifyWorkAvailable([[maybe_unused]] WorkContractGroup* group) {
    // We don't need to track which group has work, just that work is available
    _workAvailable = true;
    // Dekker pair with parkUntilWork, which sets its bit before reading the flag: an
    // empty mask proves every worker still on its way into the wait sees the store above.
    if (_parkedMaskCoversPool && _parkedMask.load(std::memory_order_seq_cst) == 0) {
        return;
    }
    _workAvailableCV.notify_one();
}

void WorkService::notifyWorkAvailableFor(WorkContractGroup* group, ExecutionType type, uint32_t lane) {
    if (type == ExecutionType::PinnedThread) {
        notifyPinnedWorkAvailable(group, lane);
        return;
    }
    notifyWorkAvailable(group);
}

// Out of line so the AnyThread path above never touches _laneWake, whose pointers
// share a cache line with the notifier-written _workAvailable.
void WorkService::notifyPinnedWorkAvailable(WorkContractGroup* group, uint32_t lane) {
    // Lanes this service does not own are drained by whatever external thread pumps
    // them; fall back to the shared wake, which is what they see today.
    if (lane >= _laneWake.size()) {
        notifyWorkAvailable(group);
        return;
    }

    // Lane L's only drainer here is worker L, and when it is itself the scheduling
    // thread it is running, not parked, and reaches the lane on its next iteration.
    if (stOwner == this && lane == stThreadId) {
        return;
    }

    // Bump after the ready bit is published. _workAvailable is deliberately left alone:
    // only lane L can claim this, so any other woken worker would just re-park.
    LaneWake& wake = _laneWake[lane];
    wake.seq.fetch_add(1, std::memory_order_acq_rel);
    if (wake.parked.load(std::memory_order_acquire) != 0) {
        { std::lock_guard<std::mutex> handshake(_workAvailableMutex); }  // see requestStop()
        _workAvailableCV.notify_all();
    }
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
    stOwner = nullptr;
}

WorkService::MainThreadWorkResult WorkService::executeMainThreadWork(size_t maxContracts) {
    // This is the per-frame "gap" — run from EntropyApplication's loop BEFORE the
    // render delegate. CpuZoneScope feeds the (now EntropyCore-resident)
    // CpuZoneProfiler so this shows in `state get cpu.zones` headlessly, and the
    // Tracy zone attributes it in the GUI timeline.
    ZoneScopedN("WorkService::executeMainThreadWork");
    ::EntropyEngine::Core::Debug::CpuZoneScope _cpuz("WorkService::executeMainThreadWork");
    MainThreadWorkResult result{0, 0, false};

    // Claim under the registry shared_lock, execute outside it. An unlocked
    // snapshot of raw group pointers would dangle if a group is removed and
    // destroyed mid-loop; claiming under the lock hands protection to the
    // group's own main-thread executing count before the pointer escapes
    // (same protocol as executeWork()).
    size_t remaining = maxContracts;
    std::vector<WorkContractGroup*> countedGroups;  // distinct groups drained (registry is small)

    while (remaining > 0) {
        WorkContractGroup* group = nullptr;
        WorkContractHandle handle;
        {
            std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);
            for (auto* candidate : _workContractGroups) {
                if (candidate && candidate->hasMainThreadWork()) {
                    handle = candidate->selectForMainThreadExecution();
                    if (handle.valid()) {
                        group = candidate;
                        break;
                    }
                }
            }
        }

        if (!group) {
            break;  // No claimable main thread work anywhere
        }

        if (std::find(countedGroups.begin(), countedGroups.end(), group) == countedGroups.end()) {
            countedGroups.push_back(group);
            result.groupsWithWork++;
        }

        // Outside the lock: the claim's executing count keeps the group alive.
        group->executeContract(handle);
        result.contractsExecuted++;
        remaining--;
    }

    // Check if there's more work available
    {
        std::shared_lock<std::shared_mutex> lock(_workContractGroupsMutex);
        for (auto* group : _workContractGroups) {
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

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
