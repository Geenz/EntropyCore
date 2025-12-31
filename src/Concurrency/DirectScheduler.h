/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file DirectScheduler.h
 * @brief Minimal overhead work scheduler for benchmarking and testing
 *
 * This file contains DirectScheduler, a bare-bones scheduler with minimum overhead
 * for benchmarking and debugging, not production use.
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
 * @brief The "just give me work!" scheduler - absolute minimum overhead.
 *
 * This is the scheduler equivalent of a greedy algorithm. It scans from the
 * beginning and grabs the first group with work. No fancy logic, no state,
 * no optimization. Just pure, simple work-finding.
 *
 * This scheduler was created to isolate scheduling logic from other system
 * overheads in benchmarking scenarios.
 *
 * The Good:
 * - No state means no memory allocation
 * - Dead simple to understand and debug
 * - First groups get priority (which might be what you want)
 *
 * The Bad:
 * - Terrible work distribution - first groups get hammered
 * - No load balancing whatsoever
 * - Later groups might starve if early groups always have work
 * - All threads pile onto the same groups
 *
 * When to use this:
 * - Benchmarking to establish absolute minimum overhead
 * - Debugging to eliminate scheduler as a variable
 * - When you have only one or two groups anyway
 * - Testing worst-case contention scenarios
 *
 * When NOT to use this:
 * - Production systems (seriously, don't)
 * - Multiple groups that need fair execution
 * - Any time you care about performance beyond raw overhead
 *
 * @code
 * // Only use this for testing!
 * auto scheduler = std::make_unique<DirectScheduler>(config);
 * // Now all threads will pile onto group[0] if it has work
 * @endcode
 */
class DirectScheduler : public IWorkScheduler
{
public:
    /**
     * @brief Constructs the world's simplest scheduler
     *
     * Config is ignored - this scheduler needs no configuration.
     *
     * @param config Ignored entirely
     */
    explicit DirectScheduler(const Config& config) {
        // No state to initialize
    }

    ~DirectScheduler() override = default;

    /**
     * @brief Finds work by scanning from the start
     *
     * Returns first group with work. All threads converge on same group -
     * bad for performance, good for measuring overhead.
     *
     * @param groups Groups to scan (in order)
     * @param context Completely ignored
     * @return First group with work, or nullptr
     */
    ScheduleResult selectNextGroup(const std::vector<WorkContractGroup*>& groups,
                                   const SchedulingContext& context) override {
        // Just scan and return first group with work
        for (auto* group : groups) {
            if (group && group->scheduledCount() > 0) {
                return {group, false};
            }
        }
        return {nullptr, true};
    }

    /**
     * @brief Returns "Direct"
     */
    const char* getName() const override {
        return "Direct";
    }
};

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
