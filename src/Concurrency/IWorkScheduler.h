/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file IWorkScheduler.h
 * @brief Abstract interface for pluggable work scheduling strategies
 *
 * This file defines the IWorkScheduler interface for pluggable scheduling algorithms.
 * Separates scheduling logic from thread management.
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

// Forward declaration
class WorkContractGroup;

/**
 * @brief Abstract interface for work scheduling strategies in the WorkService.
 *
 * IWorkScheduler defines the decision-making component that determines work execution
 * order. The WorkService manages threading and execution infrastructure while the
 * scheduler provides the selection logic. This separation enables experimentation with
 * different scheduling strategies without modifying the core thread management system.
 *
 * Implementation rationale: Custom schedulers enable optimizations for specific workload
 * patterns such as round-robin fairness, priority-based scheduling, or adaptive strategies.
 * While the default AdaptiveRankingScheduler handles most use cases effectively, specialized
 * workloads may benefit from simpler implementations with lower overhead or more targeted
 * scheduling algorithms.
 *
 * Thread Safety: Implementations MUST be thread-safe. Multiple worker threads will invoke
 * selectNextGroup() concurrently, potentially with identical group sets. Utilize atomics,
 * thread-local storage, or lock-free algorithms to ensure correctness.
 *
 * Design Requirements: selectNextGroup() executes within worker thread loops.
 * Consider the frequency of calls when designing implementations.
 *
 * @code
 * // Example custom scheduler that always picks the group with most work
 * class GreedyScheduler : public IWorkScheduler {
 * public:
 *     ScheduleResult selectNextGroup(
 *         const std::vector<WorkContractGroup*>& groups,
 *         const SchedulingContext& context) override
 *     {
 *         WorkContractGroup* best = nullptr;
 *         size_t maxWork = 0;
 *
 *         for (auto* group : groups) {
 *             size_t work = group->scheduledCount();
 *             if (work > maxWork) {
 *                 maxWork = work;
 *                 best = group;
 *             }
 *         }
 *
 *         return {best, best == nullptr};
 *     }
 *
 *     const char* getName() const override { return "Greedy"; }
 * };
 * @endcode
 */
class IWorkScheduler
{
public:
    virtual ~IWorkScheduler() = default;

    /**
     * @brief Configuration for scheduler behavior.
     *
     * Provides common configuration parameters that schedulers may utilize. Schedulers
     * can use any subset of these parameters or ignore them entirely. The WorkService
     * passes this configuration through to schedulers without modification.
     *
     * For additional configuration requirements, extend this structure or implement
     * custom configuration mechanisms specific to your scheduler implementation.
     */
    struct Config
    {
        size_t maxConsecutiveExecutionCount =
            8;  ///< How many times to execute from same group before switching (prevents starvation)
        size_t updateCycleInterval = 16;  ///< How often to refresh internal state (for adaptive schedulers)
        size_t failureSleepTime = 1;      ///< Nanoseconds to sleep when no work found (usually not needed)
        size_t threadCount = 0;           ///< Number of worker threads (0 = hardware_concurrency)
    };

    /**
     * @brief Result of a scheduling decision.
     *
     * Encapsulates the scheduler's decision for the worker thread. Indicates either
     * a selected work group or the absence of available work. The shouldSleep hint
     * guides the WorkService in choosing between spinning and backing off when no
     * work is available.
     */
    struct ScheduleResult
    {
        WorkContractGroup* group;  ///< Group to execute from (nullptr = no work available)
        bool shouldSleep;          ///< Hint: true if thread should sleep vs spin (ignored if group != nullptr)
    };

    /**
     * @brief Selects the next work group for execution
     *
     * Core scheduling method called continuously by worker threads. Examines
     * available groups and selects one for execution. Called frequently -
     * avoid allocations and complexity.
     *
     * @param groups Current snapshot of registered work groups (groups might have no work)
     * @return ScheduleResult with chosen group (or nullptr if no work found)
     *
     * @code
     * // Simplest possible implementation - just find first group with work
     * ScheduleResult selectNextGroup(groups) override {
     *     for (auto* group : groups) {
     *         if (group->scheduledCount() > 0) {
     *             return {group, false};
     *         }
     *     }
     *     return {nullptr, true};  // No work, suggest sleep
     * }
     * @endcode
     */
    virtual ScheduleResult selectNextGroup(const std::vector<WorkContractGroup*>& groups) = 0;

    /**
     * @brief Notifies scheduler that work was successfully executed
     *
     * Optional callback for tracking execution patterns. Use for adapting
     * behavior, balancing load, or maintaining fairness. Default is no-op.
     *
     * @param group The group that work was executed from
     * @param threadId The thread that executed the work
     */
    virtual void notifyWorkExecuted([[maybe_unused]] WorkContractGroup* group, [[maybe_unused]] size_t threadId) {}

    /**
     * @brief Notifies scheduler that the group list has changed
     *
     * Called when groups are added/removed. Update cached state if needed.
     * Runs concurrently with selectNextGroup() - ensure thread safety.
     *
     * @param newGroups Updated list of work groups (complete replacement)
     */
    virtual void notifyGroupsChanged([[maybe_unused]] const std::vector<WorkContractGroup*>& newGroups) {}

    /**
     * @brief Resets scheduler to initial state
     *
     * Clear all accumulated state, statistics, and learned patterns.
     * Scheduler should behave like newly constructed. Default is no-op.
     */
    virtual void reset() {}

    /**
     * @brief Gets human-readable name for this scheduling strategy
     *
     * Used in logs and debugging. Keep concise and descriptive.
     *
     * @return Name of the scheduling algorithm (must be a static string)
     */
    virtual const char* getName() const = 0;
};

/**
 * @brief Factory function type for creating schedulers.
 *
 * Enables registration of schedulers by name and dynamic switching between
 * implementations. The factory receives configuration parameters and returns
 * a new scheduler instance.
 *
 * @code
 * std::map<std::string, SchedulerFactory> schedulers = {
 *     {"round-robin", [](auto& cfg) { return std::make_unique<RoundRobinScheduler>(cfg); }},
 *     {"adaptive", [](auto& cfg) { return std::make_unique<AdaptiveRankingScheduler>(cfg); }},
 *     {"random", [](auto& cfg) { return std::make_unique<RandomScheduler>(cfg); }}
 * };
 *
 * // Create scheduler by name
 * auto scheduler = schedulers["adaptive"](config);
 * @endcode
 */
using SchedulerFactory = std::function<std::unique_ptr<IWorkScheduler>(const IWorkScheduler::Config&)>;

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
