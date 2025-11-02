/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Engine project.
 */

/**
 * @file Timer.h
 * @brief NSTimer-style timer system for scheduling delayed work
 *
 * This file contains the Timer class, which provides a high-level interface
 * for scheduling one-shot and repeating timers. Timers are backed by WorkGraph
 * nodes and integrate seamlessly with the concurrency system.
 */

#pragma once

#include <chrono>
#include <functional>
#include <atomic>
#include <memory>
#include "../Concurrency/WorkGraph.h"
#include "../Concurrency/WorkGraphTypes.h"

namespace EntropyEngine {
namespace Core {

// Forward declaration
class TimerService;

/**
 * @brief A scheduled task that executes after a delay, optionally repeating
 *
 * Timer provides an NSTimer-style interface for scheduling work with delays.
 * Timers can be one-shot (fire once) or repeating (fire at intervals). All
 * timing is handled automatically by the TimerService using yieldable WorkGraph
 * nodes.
 *
 * Key features:
 * - One-shot and repeating timers
 * - Thread-safe cancellation
 * - Main thread or background execution
 * - RAII-safe: timers invalidate on destruction
 * - No manual memory management
 *
 * Perfect for:
 * - Delayed UI updates
 * - Periodic polling
 * - Timeout handling
 * - Animation frames
 * - Network retry logic
 *
 * @code
 * // One-shot timer (fires once after delay)
 * auto timer = timerService.scheduleTimer(
 *     std::chrono::milliseconds(500),
 *     []{ LOG_INFO("Timer fired!"); },
 *     false  // One-shot
 * );
 *
 * // Repeating timer (fires every interval)
 * auto repeating = timerService.scheduleTimer(
 *     std::chrono::seconds(1),
 *     []{ updateStats(); },
 *     true  // Repeating
 * );
 *
 * // Cancel timer early
 * repeating.invalidate();
 *
 * // Main thread timer for UI updates
 * auto uiTimer = timerService.scheduleTimer(
 *     std::chrono::milliseconds(16),  // ~60 FPS
 *     []{ updateUI(); },
 *     true,  // Repeating
 *     ExecutionType::MainThread
 * );
 * @endcode
 */
class Timer {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::steady_clock::duration;
    using WorkFunction = std::function<void()>;

    /**
     * @brief Creates an invalid timer (no-op)
     *
     * Default-constructed timers do nothing and are already invalidated.
     * Use TimerService::scheduleTimer() to create active timers.
     */
    Timer() = default;

    /**
     * @brief Move constructor - transfers ownership
     *
     * The moved-from timer becomes invalid. Only one Timer can own
     * a scheduled task at a time.
     */
    Timer(Timer&& other) noexcept;

    /**
     * @brief Move assignment - transfers ownership
     *
     * Invalidates the current timer before taking ownership of the other.
     * The moved-from timer becomes invalid.
     */
    Timer& operator=(Timer&& other) noexcept;

    /**
     * @brief Destroys the timer and cancels it if still valid
     *
     * RAII cleanup - ensures timers are cancelled when they go out of scope.
     * Safe to destroy after manual invalidation.
     */
    ~Timer();

    // No copying - timers have unique ownership
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    /**
     * @brief Cancels the timer and prevents future executions
     *
     * Thread-safe. Safe to call multiple times. After invalidation,
     * the timer will not fire again. For one-shot timers, this prevents
     * the single execution. For repeating timers, this stops all future
     * repetitions.
     *
     * @code
     * auto timer = service.scheduleTimer(
     *     std::chrono::seconds(5),
     *     []{ doWork(); }
     * );
     *
     * // Changed our mind
     * timer.invalidate();  // Work will never execute
     * @endcode
     */
    void invalidate();

    /**
     * @brief Checks if the timer is still active
     *
     * @return true if the timer is valid and hasn't been cancelled
     *
     * @code
     * if (timer.isValid()) {
     *     LOG_INFO("Timer still pending");
     * } else {
     *     LOG_INFO("Timer fired or was cancelled");
     * }
     * @endcode
     */
    bool isValid() const;

    /**
     * @brief Gets the interval for repeating timers
     *
     * @return The interval duration, or zero for one-shot timers
     */
    Duration getInterval() const { return _interval; }

    /**
     * @brief Checks if this is a repeating timer
     *
     * @return true if the timer repeats, false for one-shot
     */
    bool isRepeating() const { return _repeating; }

private:
    // Only TimerService can create active timers
    friend class TimerService;

    /**
     * @brief Internal constructor used by TimerService
     *
     * @param service The service managing this timer (must outlive timer)
     * @param node WorkGraph node handle for this timer
     * @param interval Timer interval (for repeating timers)
     * @param repeating Whether this timer repeats
     */
    Timer(TimerService* service,
          Concurrency::WorkGraph::NodeHandle node,
          Duration interval,
          bool repeating);

    TimerService* _service = nullptr;
    Concurrency::WorkGraph::NodeHandle _node;
    Duration _interval{};
    bool _repeating = false;
    std::atomic<bool> _valid{false};
};

} // namespace Core
} // namespace EntropyEngine
