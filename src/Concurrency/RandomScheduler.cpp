/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "RandomScheduler.h"

#include <chrono>

#include "WorkContractGroup.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

// Thread-local state definitions
thread_local std::mt19937 RandomScheduler::stRng;
thread_local bool RandomScheduler::stRngInitialized = false;

RandomScheduler::RandomScheduler(const Config& config) {
    // Config unused for random scheduler
}

void RandomScheduler::ensureRngInitialized() {
    if (!stRngInitialized) {
        // Seed with high-resolution clock and thread ID for uniqueness
        auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        seed ^= std::hash<std::thread::id>{}(std::this_thread::get_id());
        stRng.seed(static_cast<unsigned int>(seed));
        stRngInitialized = true;
    }
}

IWorkScheduler::ScheduleResult RandomScheduler::selectNextGroup(const std::vector<WorkContractGroup*>& groups,
                                                                const SchedulingContext& context) {
    ensureRngInitialized();

    // First, count groups with work
    std::vector<WorkContractGroup*> groupsWithWork;
    groupsWithWork.reserve(groups.size());

    for (auto* group : groups) {
        if (group && group->scheduledCount() > 0) {
            groupsWithWork.push_back(group);
        }
    }

    if (groupsWithWork.empty()) {
        return {nullptr, true};
    }

    // Randomly select from groups with work
    std::uniform_int_distribution<size_t> dist(0, groupsWithWork.size() - 1);
    size_t selectedIndex = dist(stRng);

    return {groupsWithWork[selectedIndex], false};
}

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
