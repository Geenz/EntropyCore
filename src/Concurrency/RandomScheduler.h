/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file RandomScheduler.h
 * @brief Randomized work scheduler for load balancing and contention avoidance
 *
 * This file contains RandomScheduler, which uses randomization to distribute work
 * and break up contention patterns.
 */

#pragma once

#include <random>

#include "IWorkScheduler.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

/**
 * @brief The chaos monkey of schedulers - picks groups at random.
 *
 * Sometimes the best strategy is no strategy. This scheduler just rolls the dice
 * and picks a random group that has work. It's surprisingly effective at avoiding
 * certain pathological patterns that can emerge with deterministic schedulers.
 *
 * This scheduler uses a Mersenne Twister random number generator for
 * quality randomization.
 *
 * The Good:
 * - Natural load balancing - randomness spreads work evenly over time
 * - Breaks up contention patterns - threads won't fight over the same groups
 * - Simple implementation - no state to maintain or update
 * - Each thread has its own RNG - no synchronization needed
 *
 * The Not-So-Good:
 * - Zero cache locality - threads jump randomly between groups
 * - RNG computation cost
 * - Unpredictable execution order
 * - Might pick the same empty groups repeatedly (bad luck)
 *
 * When to use this:
 * - You're seeing contention with deterministic schedulers
 * - Work distribution is unpredictable or bursty
 * - You want to test if scheduling order affects your results
 * - Cache locality doesn't matter for your workload
 *
 * When NOT to use this:
 * - You need predictable, reproducible execution
 * - Cache performance is critical
 * - You have groups with vastly different work amounts
 *
 * Fun fact: Uses reservoir sampling to ensure uniform selection among groups
 * with work. Every eligible group has equal probability of being chosen.
 *
 * @code
 * // Random scheduling can help with "thundering herd" problems
 * // where all threads hit the same group at once
 * auto scheduler = std::make_unique<RandomScheduler>(config);
 * WorkService service(wsConfig, std::move(scheduler));
 *
 * // Now threads naturally spread out across groups
 * @endcode
 */
class RandomScheduler : public IWorkScheduler
{
private:
    /// Thread-local Mersenne Twister random number generator
    /// Each worker thread maintains its own RNG to avoid synchronization overhead
    /// and ensure quality randomness. Mersenne Twister provides excellent statistical
    /// properties for uniform work distribution. Thread-local because shared RNGs would
    /// require synchronization and could create correlation between threads.
    static thread_local std::mt19937 stRng;

    /// Thread-local initialization flag for lazy RNG setup
    /// Tracks whether this thread's RNG has been seeded. Each thread initializes its
    /// RNG on first use with a unique seed combining thread ID and timestamp.
    /// Thread-local because each thread needs its own initialization state.
    static thread_local bool stRngInitialized;

public:
    /**
     * @brief Constructs random scheduler
     *
     * Config is ignored. Each thread initializes its own RNG on first use.
     *
     * @param config Scheduler configuration (unused)
     */
    explicit RandomScheduler(const Config& config);

    ~RandomScheduler() override = default;

    /**
     * @brief Randomly selects a group with available work
     *
     * Uses reservoir sampling for uniform selection among eligible groups.
     * Each group with work has equal probability of being chosen.
     *
     * @param groups Available work groups
     * @param context Current thread context (ignored)
     * @return Randomly selected group with work, or nullptr if none
     *
     * @code
     * // What happens inside (simplified):
     * // 1. Start with no candidate
     * // 2. For each group with work:
     * //    - Roll dice (1 to N where N is groups seen so far)
     * //    - If we roll a 1, this becomes our candidate
     * // 3. Return final candidate
     * // This gives each group exactly 1/N probability!
     * @endcode
     */
    ScheduleResult selectNextGroup(const std::vector<WorkContractGroup*>& groups) override;

    /**
     * @brief No-op - random selection doesn't learn from history
     */
    void notifyWorkExecuted([[maybe_unused]] WorkContractGroup* group, [[maybe_unused]] size_t threadId) override {}

    /**
     * @brief No-op - random scheduler has no state to reset
     */
    void reset() override {}

    /**
     * @brief Returns "Random"
     */
    const char* getName() const override {
        return "Random";
    }

private:
    /**
     * @brief Ensures thread-local RNG is initialized
     *
     * Seeds with thread ID and time to prevent correlated sequences.
     */
    static void ensureRngInitialized();
};

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
