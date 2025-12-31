/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "RoundRobinScheduler.h"

#include "WorkContractGroup.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

// Thread-local state definition
thread_local size_t RoundRobinScheduler::stCurrentIndex = 0;

RoundRobinScheduler::RoundRobinScheduler([[maybe_unused]] const Config& config) {
    // Config mostly unused for round-robin
}

IWorkScheduler::ScheduleResult RoundRobinScheduler::selectNextGroup(const std::vector<WorkContractGroup*>& groups) {
    if (groups.empty()) {
        return {nullptr, true};
    }

    // Try each group once, starting from current position
    size_t attempts = 0;

    while (attempts < groups.size()) {
        // Wrap around if needed
        if (stCurrentIndex >= groups.size()) {
            stCurrentIndex = 0;
        }

        WorkContractGroup* group = groups[stCurrentIndex];

        // Move to next position for next call
        stCurrentIndex++;
        attempts++;

        // Check if this group has work
        if (group && group->scheduledCount() > 0) {
            return {group, false};
        }
    }

    // No groups have work
    return {nullptr, true};
}

void RoundRobinScheduler::reset() {
    stCurrentIndex = 0;
}

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
