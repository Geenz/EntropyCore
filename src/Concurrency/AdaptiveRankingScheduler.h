/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file AdaptiveRankingScheduler.h
 * @brief Smart work scheduler that learns from workload patterns and adapts distribution
 *
 * This file contains AdaptiveRankingScheduler, the default scheduler that uses adaptive
 * ranking to distribute work while maintaining cache locality and preventing starvation.
 */

#pragma once

#include <mutex>
#include <unordered_map>

#include "IWorkScheduler.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

/**
 * @brief Adaptive scheduler that learns from workload patterns to optimize distribution.
 *
 * The AdaptiveRankingScheduler serves as the default scheduler implementation. It maintains
 * thread affinity for cache locality while preventing any single group from monopolizing
 * thread resources. The scheduler functions as an adaptive load balancer that responds
 * dynamically to changing work patterns.
 *
 * The ranking algorithm: `rank = (scheduledWork / (executingWork + 1)) * (1 - executingWork / totalThreads)`
 *
 * This formula produces the following behavior:
 * - Groups with high work volume but few threads receive maximum priority
 * - Groups with existing thread allocation receive proportionally lower priority
 * - Groups consuming excessive thread resources relative to total threads are penalized
 * - Groups without pending work are excluded from consideration
 *
 * Thread affinity mechanism: Threads maintain affinity to their selected group for up to
 * maxConsecutiveExecutionCount executions. Threads relinquish affinity when the group
 * exhausts work or after reaching the consecutive execution limit.
 *
 * Each thread maintains an independent view of group rankings through thread-local
 * caching, updating only when necessary to minimize synchronization.
 *
 * Recommended use cases: Optimal for heterogeneous workloads where groups exhibit varying
 * work volumes or when work patterns change dynamically during execution.
 *
 * Not recommended when: All groups maintain equal work distribution consistently, or when
 * strict round-robin fairness is required. Consider RoundRobinScheduler for these scenarios.
 *
 * @code
 * // Configure for shorter sticky periods (more responsive to work changes)
 * IWorkScheduler::Config config;
 * config.maxConsecutiveExecutionCount = 4;  // Default is 8
 * config.updateCycleInterval = 8;           // Update rankings more often
 *
 * auto scheduler = std::make_unique<AdaptiveRankingScheduler>(config);
 * WorkService service(wsConfig, std::move(scheduler));
 * @endcode
 */
class AdaptiveRankingScheduler : public IWorkScheduler
{
private:
    Config _config;

    /**
     * @brief Per-thread state for adaptive scheduling.
     *
     * This structure enables scheduling by maintaining thread-local
     * copies of rankings and affinity state. The design eliminates locks, atomics, and
     * contention between threads. Synchronization occurs only when the group list changes
     * or during periodic rebalancing operations.
     *
     * The rankedGroups vector represents each thread's independent priority ordering.
     * Rankings update based on thread-local observations, which may differ slightly
     * between threads. This variance helps prevent thundering herd effects during
     * work distribution.
     */
    struct ThreadState
    {
        size_t currentGroupIndex = 0;                  ///< Current position in rankedGroups (thread affinity position)
        size_t consecutiveExecutionCount = 0;          ///< Number of consecutive executions on current group
        size_t rankingUpdateCounter = 0;               ///< Counts work done since last ranking update
        std::vector<WorkContractGroup*> rankedGroups;  ///< Thread-local group priority ordering
        uint64_t lastSeenGeneration = 0;               ///< Generation counter for detecting group list changes

        void reset() {
            currentGroupIndex = 0;
            consecutiveExecutionCount = 0;
            rankingUpdateCounter = 0;
            rankedGroups.clear();
            lastSeenGeneration = 0;
        }
    };

    /// Thread-local state for adaptive scheduling algorithm
    /// Each worker thread maintains its own cached rankings, sticky position, and
    /// update counters to enable lock-free scheduling decisions. This eliminates
    /// contention between threads while allowing each to adapt to the workload
    /// patterns it observes. Thread-local because each thread needs independent
    /// scheduling state to maintain the lock-free property.
    static thread_local ThreadState stThreadState;

    // Generation counter for detecting group list changes
    std::atomic<uint64_t> _groupsGeneration{0};

    /**
     * @brief Group ranking data for sorting.
     *
     * Simple struct used when computing rankings. We calculate a rank score
     * for each group and sort by it. Higher rank = higher priority.
     */
    struct GroupRank
    {
        WorkContractGroup* group;
        double rank;
    };

public:
    /**
     * @brief Constructs adaptive ranking scheduler with given configuration
     *
     * Key parameters: maxConsecutiveExecutionCount (thread stickiness),
     * updateCycleInterval (ranking refresh rate).
     *
     * @param config Scheduler configuration
     */
    explicit AdaptiveRankingScheduler(const Config& config);

    ~AdaptiveRankingScheduler() override = default;

    /**
     * @brief Selects next group using adaptive ranking algorithm
     *
     * Checks current affinity group first, then traverses ranked list.
     * Recomputes rankings when stale.
     *
     * @param groups Available work groups
     * @param context Current thread context
     * @return Selected group or nullptr if no work available
     */
    ScheduleResult selectNextGroup(const std::vector<WorkContractGroup*>& groups) override;

    /**
     * @brief Updates execution counters for affinity tracking
     *
     * Tracks consecutive executions to determine when to release affinity.
     *
     * @param group Group that work was executed from
     * @param threadId Thread that executed the work
     */
    void notifyWorkExecuted(WorkContractGroup* group, size_t threadId) override;

    /**
     * @brief Increments generation counter to invalidate cached rankings
     *
     * Threads detect generation change and update rankings. Lock-free consistency.
     *
     * @param newGroups Updated group list
     */
    void notifyGroupsChanged(const std::vector<WorkContractGroup*>& newGroups) override;

    /**
     * @brief Resets all state including thread-local data
     *
     * Only resets calling thread. Others reset lazily on next schedule.
     */
    void reset() override;

    /**
     * @brief Returns "AdaptiveRanking"
     */
    const char* getName() const override {
        return "AdaptiveRanking";
    }

private:
    /**
     * @brief Checks if current thread needs to update its rankings
     *
     * Updates when: cache empty, groups changed, interval reached, or
     * current group has no work. Balances responsiveness with overhead.
     *
     * @param groups Current group list
     * @return true if rankings should be recomputed
     */
    bool needsRankingUpdate(const std::vector<WorkContractGroup*>& groups) const;

    /**
     * @brief Updates thread-local ranking of groups based on work pressure
     *
     * Ranks by work-to-thread ratio. Excludes groups without work.
     * Different threads may have slightly different rankings - intentional.
     *
     * @param groups Groups to rank
     */
    void updateRankings(const std::vector<WorkContractGroup*>& groups);

    /**
     * @brief Executes cascading work plan through ranked groups
     *
     * Starts at affinity position, traverses ranked list. Updates affinity
     * on work discovery. Guarantees finding work if any exists.
     *
     * @param groups Available groups (for validation)
     * @return Group with available work or nullptr
     */
    WorkContractGroup* executeWorkPlan(const std::vector<WorkContractGroup*>& groups);

    /**
     * @brief Gets current affinity group if index is still valid
     *
     * Bounds-checked access to current affinity group.
     *
     * @return Current affinity group or nullptr if invalid
     */
    WorkContractGroup* getCurrentGroupIfValid() const;
};

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
