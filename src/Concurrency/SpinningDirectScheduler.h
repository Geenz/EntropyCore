/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file SpinningDirectScheduler.h
 * @brief CPU-burning scheduler for benchmarking thread wake/sleep overhead
 *
 * This file contains SpinningDirectScheduler, a diagnostic scheduler that never
 * sleeps threads. For benchmarking sleep/wake overhead.
 */

#pragma once

#include "IWorkScheduler.h"
#include "WorkContractGroup.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

/**
 * @brief CPU-intensive scheduler that eliminates sleep/wake overhead for benchmarking
 *
 * SpinningDirectScheduler extends the DirectScheduler concept by eliminating all thread
 * sleep operations. While DirectScheduler allows threads to sleep when no work is available,
 * this implementation maintains continuous CPU activity through spinning, even when work
 * queues are empty.
 *
 * Purpose: This scheduler specifically addresses the benchmarking requirement to measure
 * and isolate thread sleep/wake overhead by providing a comparison baseline where such
 * overhead is completely eliminated.
 *
 * Characteristics:
 * - CPU usage when idle: 100% per thread
 * - No thread sleep/wake cycles
 * - Threads remain active continuously
 *
 * Recommended use cases:
 * - Diagnosing impact of OS thread scheduling
 * - Measuring sleep/wake cycle overhead in workloads
 * - Testing scenarios requiring minimal latency
 * - Comparative benchmarking against DirectScheduler
 *
 * Not recommended for:
 * - Production systems (excessive CPU consumption)
 * - Battery-powered devices (rapid power drain)
 * - Shared computing environments (resource monopolization)
 * - Any scenario requiring power efficiency
 *
 * Benchmarking insight: Comparing this scheduler against DirectScheduler reveals OS-specific
 * thread wake latencies, which vary significantly by operating system and system load.
 *
 * @code
 * // Use this to compare against DirectScheduler
 * auto directScheduler = std::make_unique<DirectScheduler>(config);
 * WorkService directService(config, std::move(directScheduler));
 * // Run benchmark...
 *
 * auto spinningScheduler = std::make_unique<SpinningDirectScheduler>(config);
 * WorkService spinningService(config, std::move(spinningScheduler));
 * // Run same benchmark...
 *
 * // The difference in execution time = thread wake overhead
 * @endcode
 */
class SpinningDirectScheduler : public IWorkScheduler
{
public:
    /**
     * @brief Creates a scheduler that maintains continuous thread activity
     *
     * Config is ignored - always operates in continuous spinning mode.
     *
     * @param config Accepted for interface compatibility but unused
     *
     * @code
     * // Configuration parameters are ignored
     * IWorkScheduler::Config config;
     * config.failureSleepTime = 1000000;  // Ignored - threads will spin continuously
     * auto scheduler = std::make_unique<SpinningDirectScheduler>(config);
     * @endcode
     */
    explicit SpinningDirectScheduler(const Config& config) {}

    /**
     * @brief Destroys the scheduler
     */
    ~SpinningDirectScheduler() override = default;

    /**
     * @brief Selects the first group with work, never sleeps
     *
     * Like DirectScheduler but shouldSleep is ALWAYS false. Keeps threads
     * spinning to maintain CPU cache residency at cost of cycles.
     *
     * @param groups List of work groups to check
     * @param context Thread context (ignored)
     * @return First group with work, or {nullptr, false} to keep spinning
     *
     * @code
     * // When there's work, behaves like DirectScheduler
     * auto result = scheduler->selectNextGroup(groups, context);
     * if (result.group) {
     *     // Execute work from result.group
     * } else {
     *     // No work, but result.shouldSleep is false
     *     // Thread will immediately call selectNextGroup again
     *     // CPU core temperature increases...
     * }
     * @endcode
     */
    ScheduleResult selectNextGroup(const std::vector<WorkContractGroup*>& groups) override {
        for (auto* group : groups) {
            if (group && group->scheduledCount() > 0) {
                return {group, false};  // Never sleep
            }
        }
        // Even when no work, don't sleep - just spin
        return {nullptr, false};  // shouldSleep = false
    }

    /**
     * @brief Returns the scheduler's name
     *
     * @return "SpinningDirect"
     */
    const char* getName() const override {
        return "SpinningDirect";
    }
};

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
