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
        [timerData]() -> Concurrency::WorkResult {
            // Check if cancelled
            if (timerData->cancelled.load(std::memory_order_acquire)) {
                return Concurrency::WorkResult::Complete;
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

                    return Concurrency::WorkResult::Yield;  // Reschedule
                }

                // One-shot timer completes
                return Concurrency::WorkResult::Complete;
            }

            // Not time yet - yield and try again later
            return Concurrency::WorkResult::Yield;
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

} // namespace Core
} // namespace EntropyEngine
