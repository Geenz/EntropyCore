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
 * timer management system integrated with EntropyApplication. Each pending
 * fire is a timed work contract on the service's WorkContractGroup; it becomes
 * due at its fire time and runs on the WorkService.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Concurrency/WorkContractGroup.h"
#include "../TypeSystem/TypeID.h"
#include "EntropyService.h"
#include "Timer.h"

namespace EntropyEngine
{
namespace Core
{

// Forward declaration
namespace Concurrency
{
class WorkService;
}

/**
 * @brief Service for managing timers as timed work contracts
 *
 * TimerService provides NSTimer-style delayed execution integrated with the
 * EntropyEngine service architecture. Each pending fire is one timed work
 * contract (WorkContractHandle::scheduleAt) on the service's WorkContractGroup.
 * The contract becomes due at its fire time and runs on the WorkService. A
 * repeating timer arms its next fire from the body of the previous one.
 * TimerService owns no thread and no condition variable.
 *
 * Key features:
 * - One-shot and repeating timers
 * - Main thread or background execution
 * - Automatic integration with WorkService
 * - No polling: an idle pool wakes only for due timed contracts
 * - RAII-safe timer management
 *
 * Integration:
 * - Registered with ServiceRegistry during application startup
 * - Depends on WorkService for execution
 * - Main thread timers execute via WorkService::executeMainThreadWork()
 * - Background timers execute on worker threads
 * - A timer holds one contract slot from arm to fire; when the group is
 *   full, scheduleTimer() returns an invalid Timer
 * - With every worker inside a long contract, a due timer waits for the
 *   first worker to finish
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
class TimerService : public EntropyService
{
public:
    /**
     * @brief Configuration for the timer service
     */
    struct Config
    {
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
    const char* id() const override {
        return "com.entropy.core.timers";
    }
    const char* name() const override {
        return "TimerService";
    }
    const char* version() const override {
        return "0.1.0";
    }
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
     * @param interval Time to wait before first execution; must be positive for a repeating timer
     * @param work Function to execute when timer fires
     * @param repeating If true, timer repeats; if false, fires once
     * @param executionType Where to execute: MainThread or AnyThread
     * @return Timer handle for cancellation and status checking; an invalid Timer
     *         when the contract group has no free slot or a repeating interval is not positive
     * @throws std::runtime_error if the service is not loaded or has no WorkService
     *         (before setWorkService(), or after stop())
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
    Timer scheduleTimer(std::chrono::steady_clock::duration interval, Timer::WorkFunction work, bool repeating = false,
                        Concurrency::ExecutionType executionType = Concurrency::ExecutionType::AnyThread);

    /**
     * @brief Gets the number of currently active timers
     *
     * @return Count of timers that haven't been cancelled or completed
     */
    size_t getActiveTimerCount() const;

private:
    // Only Timer can call cancelTimer
    friend class Timer;

    /**
     * @brief Cancels a specific timer (called by Timer::invalidate)
     *
     * Thread-safe. Safe to call on already-cancelled or completed timers.
     *
     * @param timerId The TimerService-assigned id for the timer
     */
    void cancelTimer(uint64_t timerId);

    /**
     * @brief Internal timer data tracked per timer; shared with the timer's contract body
     */
    struct TimerData
    {
        uint64_t id = 0;                                   ///< TimerService-assigned id
        Timer::Duration interval{};                        ///< Interval for repeating timers
        Timer::WorkFunction work;                          ///< User's work function
        bool repeating = false;                            ///< Whether timer repeats
        Concurrency::ExecutionType executionType = Concurrency::ExecutionType::AnyThread;  ///< Where the work runs
        std::atomic<bool> cancelled{false};                ///< Cancellation flag
        std::atomic<bool> done{false};                     ///< One-shot work has run

        std::mutex mutex;                                  ///< Guards the members below
        Timer::TimePoint fireTime{};                       ///< Fire time of the pending contract
        Concurrency::WorkContractHandle pending;           ///< The armed timed contract; invalid while none is armed
        bool awaitingCapacity = false;                     ///< A re-arm found the group full
    };

    /// Arms one timed contract for @p entry at @p fireTime. The caller holds entry->mutex. On failure returns
    /// false; a created but unschedulable contract is left in @p orphan for the caller to release with no lock held.
    bool arm(const std::shared_ptr<TimerData>& entry, Timer::TimePoint fireTime,
             Concurrency::WorkContractHandle& orphan);

    /// Contract body: runs the work, then re-arms a repeating timer.
    void fire(const std::shared_ptr<TimerData>& entry);

    /// Capacity callback: re-arms entries whose re-arm found the group full.
    void onCapacityAvailable();

    /// Releases contracts left by failed arms made where release() cannot run.
    void releaseOrphans();

    Config _config;
    Concurrency::WorkContractGroup* _workContractGroup = nullptr;

    // Timer storage - protected by mutex
    mutable std::mutex _timersMutex;
    std::unordered_map<uint64_t, std::shared_ptr<TimerData>> _timers;  // timer id -> data
    uint64_t _nextTimerId = 1;                                         // protected by _timersMutex
    std::vector<Concurrency::WorkContractHandle> _orphans;             // protected by _timersMutex

    // Entries with awaitingCapacity set
    std::atomic<size_t> _awaitingCapacityCount{0};

    // WorkService reference (set during load)
    Concurrency::WorkService* _workService = nullptr;

    std::optional<Concurrency::WorkContractGroup::CapacityCallback> _capacityCallback;
};

}  // namespace Core
}  // namespace EntropyEngine
