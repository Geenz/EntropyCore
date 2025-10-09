/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include <catch2/catch_test_macros.hpp>
#include "../src/Concurrency/WorkContractGroup.h"
#include <atomic>
#include <vector>
#include <thread>

using namespace EntropyEngine::Core::Concurrency;

TEST_CASE("WorkContractGroup re-entrance: fan-out within same group", "[workcontract][reentrance][fanout]") {
    // Choose capacity equal to desired children so that without re-entrance
    // (parent slot not freed before execution), one child would fail.
    const size_t capacity = 8;
    const int children = static_cast<int>(capacity);
    WorkContractGroup group(capacity);

    std::atomic<int> createdChildren{0};
    std::atomic<int> createdFailures{0};
    std::atomic<int> executedChildren{0};

    auto parent = group.createContract([&]() {
        // Create and schedule 'children' tasks re-entrantly from within the parent's execution.
        // This should succeed for all when the parent's slot is returned to the freelist before task() runs.
        for (int i = 0; i < children; ++i) {
            auto child = group.createContract([&executedChildren]() {
                executedChildren.fetch_add(1, std::memory_order_relaxed);
            });
            if (child.valid()) {
                createdChildren.fetch_add(1, std::memory_order_relaxed);
                child.schedule();
            } else {
                createdFailures.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Important: do not call group.wait() here to avoid waiting on ourselves (executingCount includes parent).
    });

    REQUIRE(parent.valid());
    REQUIRE(parent.schedule() == ScheduleResult::Scheduled);

    // Drain all background work in the calling thread. This will execute the parent first,
    // then the scheduled children.
    group.executeAllBackgroundWork();

    REQUIRE(createdFailures.load(std::memory_order_relaxed) == 0);
    REQUIRE(createdChildren.load(std::memory_order_relaxed) == children);
    REQUIRE(executedChildren.load(std::memory_order_relaxed) == children);

    // Final state sanity
    REQUIRE(group.scheduledCount() == 0);
    REQUIRE(group.executingCount() == 0);
    REQUIRE(group.activeCount() == 0);
}

TEST_CASE("WorkContractGroup re-entrance: recursive creation within same group", "[workcontract][reentrance][recursive]") {
    // Provide enough capacity for a small binary tree of tasks.
    WorkContractGroup group(128);

    std::atomic<int> created{0};
    std::atomic<int> executed{0};

    // Depth-limited binary recursion, created from within executing tasks.
    const int maxDepth = 3; // produces up to 1 + 2 + 4 + 8 = 15 tasks worst-case

    std::function<void(int)> makeWork = [&](int depth) {
        executed.fetch_add(1, std::memory_order_relaxed);
        if (depth < maxDepth) {
            for (int i = 0; i < 2; ++i) {
                auto h = group.createContract([&, depth]() {
                    makeWork(depth + 1);
                });
                if (h.valid()) {
                    created.fetch_add(1, std::memory_order_relaxed);
                    h.schedule();
                }
            }
        }
    };

    auto root = group.createContract([&]() { makeWork(0); });
    REQUIRE(root.valid());
    REQUIRE(root.schedule() == ScheduleResult::Scheduled);

    // Drain all work; recursive children are scheduled during execution of their parents.
    group.executeAllBackgroundWork();

    // All created tasks should have executed.
    REQUIRE(executed.load(std::memory_order_relaxed) == created.load(std::memory_order_relaxed));

    // Final group state should be idle
    REQUIRE(group.scheduledCount() == 0);
    REQUIRE(group.executingCount() == 0);
    REQUIRE(group.activeCount() == 0);
}
