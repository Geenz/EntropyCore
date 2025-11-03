/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Engine project.
 */

#include "TimerService.h"
#include "../Concurrency/WorkService.h"
#include <chrono>

namespace EntropyEngine {
namespace Core {

TimerService::TimerService()
    : TimerService(Config{}) {
}

TimerService::TimerService(const Config& config)
    : _config(config) {
}

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
    _workContractGroup = std::make_unique<Concurrency::WorkContractGroup>(_config.workContractGroupSize);

    // Create WorkGraph using the WorkContractGroup
    _workGraph = std::make_unique<Concurrency::WorkGraph>(_workContractGroup.get());
}

void TimerService::start() {
    // Note: WorkService must be injected via setWorkService() before calling start()
    // The application is responsible for injecting dependencies
}

void TimerService::stop() {
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
        for (auto& [index, timerData] : _timers) {
            timerData->cancelled.store(true, std::memory_order_release);
        }
    }

    // Suspend the WorkGraph to prevent rescheduling
    if (_workGraph) {
        _workGraph->suspend();
    }

    // Unregister WorkContractGroup from WorkService
    if (_workService && _workContractGroup) {
        _workService->removeWorkContractGroup(_workContractGroup.get());
    }
}

void TimerService::unload() {
    // Clear all timer data
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        _timers.clear();
    }

    // Clean up WorkGraph and WorkContractGroup
    _workGraph.reset();
    _workContractGroup.reset();
    _workService = nullptr;
}

void TimerService::setWorkService(Concurrency::WorkService* workService) {
    _workService = workService;

    // Register our WorkContractGroup with the WorkService
    if (_workService && _workContractGroup) {
        auto status = _workService->addWorkContractGroup(_workContractGroup.get());
        if (status != Concurrency::WorkService::GroupOperationStatus::Added) {
            throw std::runtime_error("Failed to register TimerService WorkContractGroup with WorkService");
        }

        // Start the WorkGraph execution
        if (_workGraph) {
            _workGraph->execute();
        }

        // Start the background pump contract
        // Runs on AnyThread to avoid monopolizing main thread queue
        // Main thread timers will still execute on main thread when ready
        restartPumpContract();
    }
}

Timer TimerService::scheduleTimer(std::chrono::steady_clock::duration interval,
                                 Timer::WorkFunction work,
                                 bool repeating,
                                 Concurrency::ExecutionType executionType) {
    if (!_workGraph) {
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

    // Create yieldable node that checks elapsed time
    auto node = _workGraph->addYieldableNode(
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
        "Timer",
        nullptr,
        executionType,
        std::nullopt  // No max reschedules for timers
    );

    // Store timer data
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        _timers[node.handleIndex()] = timerData;
    }

    // Ensure pump contract is running (thread-safe)
    restartPumpContract();

    // Return Timer handle
    return Timer(this, node, interval, repeating);
}

void TimerService::cancelTimer(Concurrency::WorkGraph::NodeHandle node) {
    std::lock_guard<std::mutex> lock(_timersMutex);
    auto it = _timers.find(node.handleIndex());
    if (it != _timers.end()) {
        it->second->cancelled.store(true, std::memory_order_release);
    }
}

size_t TimerService::getActiveTimerCount() const {
    std::lock_guard<std::mutex> lock(_timersMutex);
    size_t activeCount = 0;
    for (const auto& [index, timerData] : _timers) {
        if (!timerData->cancelled.load(std::memory_order_acquire)) {
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

        // Always reschedule to ensure pump is running for new timers
        // This prevents race condition where pump stops before timers are added
        std::lock_guard<std::mutex> contractLock(_pumpContractMutex);
        if (_workContractGroup) {
            // Lock the weak_ptr to ensure pump function is still alive
            auto pumpFunction = weakPump.lock();
            if (!pumpFunction) return; // Already destroyed, stop rescheduling

            _pumpContractHandle = _workContractGroup->createContract(
                *pumpFunction,
                Concurrency::ExecutionType::AnyThread
            );
            _pumpContractHandle.schedule();
        }
        // Execution mutex released here - stop() can now proceed
    };

    // Schedule initial execution on background thread
    _pumpContractHandle = _workContractGroup->createContract(
        *_pumpFunction,
        Concurrency::ExecutionType::AnyThread
    );
    _pumpContractHandle.schedule();
}

size_t TimerService::processReadyTimers() {
    if (_workGraph) {
        return _workGraph->checkTimedDeferrals();
    }
    return 0;
}

} // namespace Core
} // namespace EntropyEngine
