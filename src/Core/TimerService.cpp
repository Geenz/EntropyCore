/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Engine project.
 */

#include "TimerService.h"

#include <chrono>

#include "../Concurrency/WorkService.h"

namespace EntropyEngine
{
namespace Core
{

TimerService::TimerService() : TimerService(Config{}) {}

TimerService::TimerService(const Config& config) : _config(config) {}

TimerService::~TimerService() {
    // Ensure clean shutdown
    if (state() != ServiceState::Unloaded && state() != ServiceState::Stopped) {
        stop();
        unload();
    }
}

std::vector<TypeSystem::TypeID> TimerService::dependsOnTypes() const {
    return {TypeSystem::createTypeId<Concurrency::WorkService>()};
}

void TimerService::load() {
    // Create WorkContractGroup for timer nodes
    // Note: Initial refcount=1 is set by EntropyObject base class (see EntropyObject.h:56)
    _workContractGroup = new Concurrency::WorkContractGroup(_config.workContractGroupSize);

    // Timer graphs are created per-timer in scheduleTimer(); graphs are
    // build-once-execute-many, so a shared perpetual graph is not an option.
}

void TimerService::start() {
    // Note: WorkService must be injected via setWorkService() before calling start()
    // The application is responsible for injecting dependencies
}

void TimerService::stop() {
    // Early exit if already unloaded (WorkContractGroup destroyed)
    if (!_workContractGroup) {
        return;
    }

    // Step 1: Signal pump to stop (like WorkService::requestStop)
    _pumpShouldStop.store(true, std::memory_order_release);

    // Step 2: Cancel the pump contract to prevent new schedules
    {
        std::lock_guard<std::mutex> lock(_pumpContractMutex);
        if (_pumpContractHandle.valid()) {
            _pumpContractHandle.release();
        }
        // Clear the pump function to break any weak_ptr references
        _pumpFunction.reset();
    }

    // Step 3: Wait for any in-flight pump execution (like WorkService::waitForStop)
    // Acquiring this mutex blocks until pump releases it
    {
        std::lock_guard<std::mutex> lock(_pumpExecutionMutex);
        // Pump is now guaranteed to be idle
    }

    // Step 4: Now safe to cleanup - no pump can be running
    // Cancel all active timers
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        for (auto& [id, entry] : _timers) {
            entry.data->cancelled.store(true, std::memory_order_release);
        }
    }

    // Unregister WorkContractGroup from WorkService
    if (_workService && _workContractGroup) {
        _workService->removeWorkContractGroup(_workContractGroup);
    }
}

void TimerService::unload() {
    // Destroy all timer entries. Graphs with pending (cancelled-but-unfired)
    // nodes drain themselves in ~WorkGraph; that is the ordinary shutdown path
    // for any timer that has not reached its fire time. The group must still be
    // alive here because each graph destructor unregisters its callbacks from it.
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        _timers.clear();
    }

    // Release our reference to the WorkContractGroup
    // Object will be deleted when all references are released (including WorkService snapshots)
    if (_workContractGroup) {
        _workContractGroup->release();
        _workContractGroup = nullptr;
    }

    _workService = nullptr;
}

void TimerService::setWorkService(Concurrency::WorkService* workService) {
    _workService = workService;

    // Register our WorkContractGroup with the WorkService
    if (_workService && _workContractGroup) {
        auto status = _workService->addWorkContractGroup(_workContractGroup);
        if (status != Concurrency::WorkService::GroupOperationStatus::Added) {
            throw std::runtime_error("Failed to register TimerService WorkContractGroup with WorkService");
        }

        // Start the background pump contract
        // Runs on AnyThread to avoid monopolizing main thread queue
        // Main thread timers will still execute on main thread when ready
        restartPumpContract();
    }
}

Timer TimerService::scheduleTimer(std::chrono::steady_clock::duration interval, Timer::WorkFunction work,
                                  bool repeating, Concurrency::ExecutionType executionType) {
    if (!_workContractGroup) {
        throw std::runtime_error("TimerService not loaded");
    }

    if (!_workService) {
        throw std::runtime_error("TimerService not started - WorkService not set");
    }

    // Create timer data
    auto timerData = std::make_shared<TimerData>();
    timerData->fireTime = std::chrono::steady_clock::now() + interval;
    timerData->interval = interval;
    timerData->work = std::move(work);
    timerData->repeating = repeating;

    // One small graph per timer: a single yieldable node that does the waiting
    // via yieldUntil. Graphs are build-once-execute-many; the old design of one
    // perpetual graph gaining nodes forever mutated a running graph.
    Concurrency::WorkGraphConfig graphConfig;
    graphConfig.enableEvents = false;
    graphConfig.enableDebugRegistration = false;
    auto graph = std::make_unique<Concurrency::WorkGraph>(_workContractGroup, graphConfig);

    // Create yieldable node that checks elapsed time
    graph->addYieldableNode(
        [timerData]() -> Concurrency::WorkResultContext {
            // Check if cancelled
            if (timerData->cancelled.load(std::memory_order_acquire)) {
                return Concurrency::WorkResultContext::complete();
            }

            // Check if enough time has elapsed
            auto now = std::chrono::steady_clock::now();
            if (now >= timerData->fireTime) {
                // Execute user's work
                if (timerData->work) {
                    timerData->work();
                }

                // For repeating timers, update fire time and reschedule
                if (timerData->repeating && !timerData->cancelled.load(std::memory_order_acquire)) {
                    // Use absolute time tracking to prevent drift accumulation
                    // Skip any missed intervals to avoid rapid catch-up firing (like NSTimer)
                    do {
                        timerData->fireTime += timerData->interval;
                    } while (timerData->fireTime <= now);

                    // Yield until next fire time - NO BUSY WAITING!
                    return Concurrency::WorkResultContext::yieldUntil(timerData->fireTime);
                }

                // One-shot timer completes
                return Concurrency::WorkResultContext::complete();
            }

            // Not time yet - yield until fire time instead of immediate reschedule
            return Concurrency::WorkResultContext::yieldUntil(timerData->fireTime);
        },
        "Timer", nullptr, executionType,
        std::nullopt  // No max reschedules for timers
    );

    // Start the timer's graph; the node runs once immediately and yields until
    // its fire time.
    graph->execute();

    // Store the entry
    uint64_t timerId;
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        timerId = _nextTimerId++;
        _timers[timerId] = TimerEntry{std::move(graph), timerData};
    }

    // Ensure pump contract is running (thread-safe)
    restartPumpContract();

    // Return Timer handle
    return Timer(this, timerId, interval, repeating);
}

void TimerService::cancelTimer(uint64_t timerId) {
    std::lock_guard<std::mutex> lock(_timersMutex);
    auto it = _timers.find(timerId);
    if (it != _timers.end()) {
        it->second.data->cancelled.store(true, std::memory_order_release);
    }
}

size_t TimerService::getActiveTimerCount() const {
    std::lock_guard<std::mutex> lock(_timersMutex);
    size_t activeCount = 0;
    for (const auto& [id, entry] : _timers) {
        if (!entry.data->cancelled.load(std::memory_order_acquire) && entry.graph && !entry.graph->isComplete()) {
            ++activeCount;
        }
    }
    return activeCount;
}

void TimerService::restartPumpContract() {
    // Thread-safe check and restart of pump contract
    std::lock_guard<std::mutex> lock(_pumpContractMutex);

    // Check if pump is already running or stopping
    if (_pumpContractHandle.valid() || !_workContractGroup || _pumpShouldStop.load(std::memory_order_acquire)) {
        return;
    }

    // Create self-rescheduling pump function (stored as member to keep weak_ptr valid)
    _pumpFunction = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakPump = _pumpFunction;
    *_pumpFunction = [this, weakPump]() {
        // Hold execution mutex for entire pump execution (synchronous cleanup pattern)
        std::lock_guard<std::mutex> execLock(_pumpExecutionMutex);

        // Check stop flag at start - abort if stopping
        if (_pumpShouldStop.load(std::memory_order_acquire)) {
            return;
        }

        // Safe to access TimerService members now - stop() is blocked
        processReadyTimers();

        // Check stop flag again before rescheduling
        if (_pumpShouldStop.load(std::memory_order_acquire)) {
            return;
        }

        // Reschedule pump to continue checking for ready timers
        // This prevents race where timers are added after pump completes but before rescheduling
        std::lock_guard<std::mutex> contractLock(_pumpContractMutex);
        if (_workContractGroup) {
            // Lock the weak_ptr to ensure pump function is still alive
            auto pumpFunction = weakPump.lock();
            if (!pumpFunction) {
                // Pump function released during shutdown, stop rescheduling
                _pumpContractHandle = Concurrency::WorkContractHandle();  // Reset to invalid handle
                return;
            }

            _pumpContractHandle =
                _workContractGroup->createContract(*pumpFunction, Concurrency::ExecutionType::AnyThread);
            _pumpContractHandle.schedule();
        }
        // Execution mutex released here - stop() can now proceed
    };

    // Schedule initial execution on background thread
    _pumpContractHandle = _workContractGroup->createContract(*_pumpFunction, Concurrency::ExecutionType::AnyThread);
    _pumpContractHandle.schedule();
}

size_t TimerService::processReadyTimers() {
    // Wake any timers whose fire time has arrived, and reap graphs whose
    // one-shot (or cancelled) timer has completed. checkTimedDeferrals() is
    // also invoked per-graph by WorkService idle workers via the group's
    // timed-deferral callback list; this pump guarantees progress even when
    // workers are busy, and is the only place completed entries are erased.
    std::lock_guard<std::mutex> lock(_timersMutex);
    size_t scheduled = 0;
    for (auto it = _timers.begin(); it != _timers.end();) {
        auto& entry = it->second;
        if (entry.graph && entry.graph->isComplete()) {
            it = _timers.erase(it);  // Timer fired its last (or was cancelled and drained)
            continue;
        }
        if (entry.graph) {
            scheduled += entry.graph->checkTimedDeferrals();
        }
        ++it;
    }
    return scheduled;
}

}  // namespace Core
}  // namespace EntropyEngine
