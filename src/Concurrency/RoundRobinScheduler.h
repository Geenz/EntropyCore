/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file RoundRobinScheduler.h
 * @brief Fair round-robin work scheduler for uniform load distribution
 *
 * This file contains RoundRobinScheduler, which cycles through work groups in order
 * for fair scheduling with predictable behavior.
 */

#pragma once

#include <atomic>

#include "IWorkScheduler.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

/**
 * @brief Fair round-robin scheduler providing uniform work distribution
 *
 * RoundRobinScheduler implements a simple circular scheduling strategy where each
 * thread cycles through work groups in sequential order. Upon reaching the last
 * group, the scheduler wraps back to the first, ensuring equal opportunity for
 * all groups. This approach prioritizes fairness and simplicity over adaptive
 * optimization.
 *
 *
 * Advantages:
 * - Simple implementation facilitates understanding and debugging
 * - Provides perfect fairness with equal execution opportunities
 * - No complex calculations or state management
 * - Zero contention through thread-local counter implementation
 *
 * Limitations:
 * - No empty group detection (cycles through all groups regardless of work availability)
 * - Disregards cache locality (threads alternate between groups)
 * - Cannot adapt to workload imbalances (uniform treatment of all groups)
 *
 * Recommended use cases:
 * - Workloads with similar work distribution across groups
 * - Systems requiring predictable, deterministic scheduling behavior
 * - Debugging scenarios where scheduler variability must be eliminated
 * - Applications prioritizing fairness over throughput
 *
 * Not recommended when:
 * - Significant work imbalances exist between groups
 * - Work arrives in bursts to specific groups
 * - Cache locality is important
 *
 * @code
 * // Perfect for evenly distributed work
 * for (auto& group : groups) {
 *     for (int i = 0; i < 100; ++i) {
 *         auto handle = group->createContract(work);
 *         handle.schedule();
 *     }
 * }
 *
 * // Use round-robin for predictable execution
 * auto scheduler = std::make_unique<RoundRobinScheduler>(config);
 * WorkService service(wsConfig, std::move(scheduler));
 * @endcode
 */
class RoundRobinScheduler : public IWorkScheduler
{
private:
    /// Thread-local position in the round-robin rotation
    /// Each worker thread maintains its own position in the group list to ensure
    /// fair round-robin distribution without synchronization overhead. Threads naturally
    /// spread out across different starting positions, providing good load balancing.
    /// Thread-local because each thread needs its own independent rotation state.
    static thread_local size_t stCurrentIndex;

public:
    /**
     * @brief Constructs round-robin scheduler
     *
     * Config is unused - round-robin needs no tuning.
     *
     * @param config Scheduler configuration (unused)
     */
    explicit RoundRobinScheduler(const Config& config);

    ~RoundRobinScheduler() override = default;

    /**
     * @brief Selects next group in round-robin order
     *
     * Starts where we left off, checks each group for work. Each thread
     * maintains its own position - no synchronization needed.
     *
     * @param groups Available work groups
     * @param context Current thread context (ignored)
     * @return Next group with work, or nullptr if none
     *
     * @code
     * // What actually happens inside:
     * // 1. Get this thread's current position
     * // 2. Loop through all groups starting from that position
     * // 3. First group with scheduledCount() > 0 wins
     * // 4. Update position for next time
     * // 5. Return the winner (or nullptr)
     * @endcode
     */
    ScheduleResult selectNextGroup(const std::vector<WorkContractGroup*>& groups) override;

    /**
     * @brief No-op - round-robin doesn't track execution history
     */
    void notifyWorkExecuted([[maybe_unused]] WorkContractGroup* group, [[maybe_unused]] size_t threadId) override {}

    /**
     * @brief Resets thread-local rotation index to 0
     *
     * Only affects calling thread. Others reset on next schedule.
     */
    void reset() override;

    /**
     * @brief Returns "RoundRobin"
     */
    const char* getName() const override {
        return "RoundRobin";
    }
};

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
