/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Engine project.
 */

/**
 * @file TimerService.h
 * @brief Service for scheduling delayed and repeating timers
 *
 * This file contains the TimerService class, which provides a centralized
 * timer management system integrated with EntropyApplication. Timers are
 * implemented using WorkGraph yieldable nodes for efficient scheduling.
 */

#pragma once

#include <memory>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include "EntropyService.h"
#include "Timer.h"
#include "../Concurrency/WorkGraph.h"
#include "../Concurrency/WorkContractGroup.h"
#include "../TypeSystem/TypeID.h"

namespace EntropyEngine {
namespace Core {

// Forward declaration
namespace Concurrency {
    class WorkService;
}

/**
 * @brief Service for managing timers with WorkGraph backing
 *
 * TimerService provides NSTimer-style delayed execution integrated with the
 * EntropyEngine service architecture. All timers are backed by yieldable
 * WorkGraph nodes, allowing efficient scheduling without dedicated timer threads.
 *
 * Key features:
 * - One-shot and repeating timers
 * - Main thread or background execution
 * - Automatic integration with WorkService
 * - No polling overhead - uses yieldable nodes
 * - RAII-safe timer management
 *
 * Integration:
 * - Registered with ServiceRegistry during application startup
 * - Depends on WorkService for execution
 * - Main thread timers execute via WorkService::executeMainThreadWork()
 * - Background timers execute on worker threads
 *
 * Perfect for:
 * - Delayed UI updates
 * - Periodic polling
 * - Timeout handling
 * - Animation timing
 * - Retry logic
 *
 * @code
 * // In EntropyApplication setup
 * auto timerService = std::make_shared<TimerService>();
 * services.registerService<TimerService>(timerService);
 *
 * // Later, from any system
 * auto& timers = app.getServices().get<TimerService>();
 *
 * // One-shot timer
 * auto timer = timers->scheduleTimer(
 *     std::chrono::seconds(5),
 *     []{ LOG_INFO("5 seconds elapsed"); }
 * );
 *
 * // Repeating timer on main thread
 * auto frameTimer = timers->scheduleTimer(
 *     std::chrono::milliseconds(16),
 *     []{ updateFrame(); },
 *     true,  // Repeating
 *     ExecutionType::MainThread
 * );
 *
 * // Cancel timer early
 * frameTimer.invalidate();
 * @endcode
 */
class TimerService : public EntropyService {
public:
    /**
     * @brief Configuration for the timer service
     */
    struct Config {
        size_t workContractGroupSize = 1024;  ///< Size of internal work contract pool
    };

    /**
     * @brief Creates a timer service with default configuration
     */
    TimerService();

    /**
     * @brief Creates a timer service with custom configuration
     *
     * @param config Service configuration
     */
    explicit TimerService(const Config& config);

    /**
     * @brief Destroys the timer service and cancels all active timers
     */
    ~TimerService() override;

    // EntropyService interface
    const char* id() const override { return "com.entropy.core.timers"; }
    const char* name() const override { return "TimerService"; }
    const char* version() const override { return "0.1.0"; }
    TypeSystem::TypeID typeId() const override {
        return TypeSystem::createTypeId<TimerService>();
    }
    std::vector<TypeSystem::TypeID> dependsOnTypes() const override;
    std::vector<std::string> dependsOn() const override {
        return {"com.entropy.core.work"};
    }

    void load() override;
    void start() override;
    void stop() override;
    void unload() override;

    /**
     * @brief Sets the WorkService reference (must be called before start)
     *
     * This method allows the application to inject the WorkService dependency
     * after both services have been created. Must be called after load() and
     * before start().
     *
     * @param workService The WorkService to use for timer execution
     * @throws std::runtime_error if WorkContractGroup registration fails
     */
    void setWorkService(Concurrency::WorkService* workService);

    /**
     * @brief Schedules a timer to execute after a delay
     *
     * Creates a new timer that executes the provided work function after the
     * specified interval. For repeating timers, the work executes repeatedly
     * at the interval until cancelled.
     *
     * Thread-safe. Can be called from any thread. The work function will
     * execute on the specified execution context (main thread or worker threads).
     *
     * @param interval Time to wait before first execution
     * @param work Function to execute when timer fires
     * @param repeating If true, timer repeats; if false, fires once
     * @param executionType Where to execute: MainThread or AnyThread
     * @return Timer handle for cancellation and status checking
     *
     * @code
     * // One-shot timeout
     * auto timeout = service.scheduleTimer(
     *     std::chrono::seconds(30),
     *     []{ handleTimeout(); },
     *     false  // One-shot
     * );
     *
     * // Repeating poll every 100ms
     * auto poll = service.scheduleTimer(
     *     std::chrono::milliseconds(100),
     *     []{ checkStatus(); },
     *     true  // Repeating
     * );
     *
     * // Main thread UI update at 60 FPS
     * auto frameUpdate = service.scheduleTimer(
     *     std::chrono::milliseconds(16),
     *     []{ renderFrame(); },
     *     true,  // Repeating
     *     ExecutionType::MainThread
     * );
     * @endcode
     */
    Timer scheduleTimer(std::chrono::steady_clock::duration interval,
                       Timer::WorkFunction work,
                       bool repeating = false,
                       Concurrency::ExecutionType executionType = Concurrency::ExecutionType::AnyThread);

    /**
     * @brief Gets the number of currently active timers
     *
     * @return Count of timers that haven't been cancelled or completed
     */
    size_t getActiveTimerCount() const;

    /**
     * @brief Checks for ready timers and schedules them for execution
     *
     * Examines timers that are waiting for their scheduled time and wakes up
     * any whose time has arrived. Call this periodically from your main loop
     * to ensure timers fire promptly, especially when the system is idle.
     *
     * @return Number of timers that were woken up and scheduled
     *
     * @code
     * // In main loop
     * while (running) {
     *     timerService->processReadyTimers();
     *     // ... other work ...
     *     std::this_thread::sleep_for(10ms);
     * }
     * @endcode
     */
    size_t processReadyTimers();

private:
    // Only Timer can call cancelTimer
    friend class Timer;

    /**
     * @brief Cancels a specific timer (called by Timer::invalidate)
     *
     * Thread-safe. Safe to call on already-cancelled timers.
     *
     * @param node The WorkGraph node handle for the timer
     */
    void cancelTimer(Concurrency::WorkGraph::NodeHandle node);

    /**
     * @brief Internal timer data tracked per node
     */
    struct TimerData {
        Timer::TimePoint fireTime;      ///< When timer should fire
        Timer::Duration interval;       ///< Interval for repeating timers
        Timer::WorkFunction work;       ///< User's work function
        bool repeating;                 ///< Whether timer repeats
        std::atomic<bool> cancelled{false};  ///< Cancellation flag
    };

    Config _config;
    std::unique_ptr<Concurrency::WorkContractGroup> _workContractGroup;
    std::unique_ptr<Concurrency::WorkGraph> _workGraph;

    // Timer data storage - protected by mutex
    mutable std::mutex _timersMutex;
    std::unordered_map<uint32_t, std::shared_ptr<TimerData>> _timers;  // node index -> timer data

    // WorkService reference (set during load)
    Concurrency::WorkService* _workService = nullptr;

    // Smart pump contract - only reschedules when active timers exist
    Concurrency::WorkContractHandle _pumpContractHandle;
};

} // namespace Core
} // namespace EntropyEngine
