/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "AdaptiveRankingScheduler.h"

#include <algorithm>
#include <cmath>

#include "WorkContractGroup.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

// Thread-local state definition
thread_local AdaptiveRankingScheduler::ThreadState AdaptiveRankingScheduler::stThreadState;

AdaptiveRankingScheduler::AdaptiveRankingScheduler(const Config& config) : _config(config) {}

IWorkScheduler::ScheduleResult AdaptiveRankingScheduler::selectNextGroup(
    const std::vector<WorkContractGroup*>& groups) {
    // Phase 1: Try to execute from the current sticky group for cache locality
    if (stThreadState.consecutiveExecutionCount < _config.maxConsecutiveExecutionCount) {
        WorkContractGroup* stickyGroup = getCurrentGroupIfValid();
        if (stickyGroup && stickyGroup->scheduledCount() > 0) {
            return {stickyGroup, false};
        }
    }

    // Phase 2: Sticky state is broken. Find a new work plan
    stThreadState.consecutiveExecutionCount = 0;

    if (needsRankingUpdate(groups)) {
        updateRankings(groups);
    }

    // Phase 3: Execute the new work plan
    WorkContractGroup* selectedGroup = executeWorkPlan(groups);

    if (selectedGroup) {
        return {selectedGroup, false};
    }

    // No work found anywhere
    return {nullptr, true};
}

void AdaptiveRankingScheduler::notifyWorkExecuted(WorkContractGroup* group, size_t threadId) {
    stThreadState.consecutiveExecutionCount++;
    stThreadState.rankingUpdateCounter++;
}

void AdaptiveRankingScheduler::notifyGroupsChanged(const std::vector<WorkContractGroup*>& newGroups) {
    _groupsGeneration.fetch_add(1, std::memory_order_relaxed);
}

void AdaptiveRankingScheduler::reset() {
    stThreadState.reset();
    _groupsGeneration = 0;
}

bool AdaptiveRankingScheduler::needsRankingUpdate(const std::vector<WorkContractGroup*>& groups) const {
    // Update rankings if:
    // 1. Never computed (empty cache)
    if (stThreadState.rankedGroups.empty()) {
        return true;
    }

    // 2. Group list has changed (generation mismatch)
    uint64_t currentGeneration = _groupsGeneration.load(std::memory_order_relaxed);
    if (stThreadState.lastSeenGeneration != currentGeneration) {
        return true;
    }

    // 3. Update interval reached
    if (stThreadState.rankingUpdateCounter >= _config.updateCycleInterval) {
        return true;
    }

    // 4. Current sticky group has no more work
    WorkContractGroup* currentGroup = getCurrentGroupIfValid();
    if (currentGroup && currentGroup->scheduledCount() == 0) {
        return true;
    }

    return false;
}

void AdaptiveRankingScheduler::updateRankings(const std::vector<WorkContractGroup*>& groups) {
    std::vector<GroupRank> rankings;

    // Calculate rankings for each group
    for (auto* group : groups) {
        if (!group) continue;

        size_t scheduled = group->scheduledCount();
        if (scheduled == 0) continue;  // Skip groups with no work

        size_t executing = group->executingCount();

        // Using the SRS formula with floating point math
        double executionCountF = static_cast<double>(executing) + 1.0;
        double scheduleCountF = static_cast<double>(scheduled);
        double threadCountF = static_cast<double>(_config.threadCount);

        double threadPenalty = 1.0 - (executionCountF / threadCountF);
        double rank = (scheduleCountF / executionCountF) * threadPenalty;

        rankings.push_back({group, rank});
    }

    // Sort by rank (highest first)
    std::sort(rankings.begin(), rankings.end(), [](const GroupRank& a, const GroupRank& b) { return a.rank > b.rank; });

    // Update cached ordered groups
    stThreadState.rankedGroups.clear();
    stThreadState.rankedGroups.reserve(rankings.size());
    for (const auto& r : rankings) {
        stThreadState.rankedGroups.push_back(r.group);
    }

    // Reset update counter and current sticky group index
    stThreadState.rankingUpdateCounter = 0;
    stThreadState.currentGroupIndex = 0;

    // Record the generation we've seen
    stThreadState.lastSeenGeneration = _groupsGeneration.load(std::memory_order_relaxed);
}

WorkContractGroup* AdaptiveRankingScheduler::executeWorkPlan(const std::vector<WorkContractGroup*>& groups) {
    // Iterate through the ranked list of groups (the "plan")
    for (size_t i = 0; i < stThreadState.rankedGroups.size(); ++i) {
        WorkContractGroup* group = stThreadState.rankedGroups[i];
        if (!group) continue;

        // Verify group is still in the main list (could have been removed)
        auto it = std::find(groups.begin(), groups.end(), group);
        if (it == groups.end()) continue;

        if (group->scheduledCount() > 0) {
            // Success! We found work
            // Set this as our new sticky group for the next loop
            stThreadState.currentGroupIndex = i;
            stThreadState.consecutiveExecutionCount = 1;
            return group;
        }
        // If no work, cascade to the next group in the list
    }

    // We iterated through the entire plan and found no work
    return nullptr;
}

WorkContractGroup* AdaptiveRankingScheduler::getCurrentGroupIfValid() const {
    if (stThreadState.currentGroupIndex >= stThreadState.rankedGroups.size()) {
        return nullptr;
    }
    return stThreadState.rankedGroups[stThreadState.currentGroupIndex];
}

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
