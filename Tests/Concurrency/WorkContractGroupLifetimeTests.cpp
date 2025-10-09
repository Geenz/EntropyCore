/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include <catch2/catch_test_macros.hpp>
#include "../src/Concurrency/WorkService.h"
#include "../src/Concurrency/WorkContractGroup.h"

using namespace EntropyEngine::Core::Concurrency;

TEST_CASE("WorkContractGroup lifetime semantics with WorkService (stack vs heap)") {
    SECTION("Stack-allocated group: service retains and auto-removal via destructor notification") {
        WorkService service{WorkService::Config{}};
        service.start();
        {
            WorkContractGroup group(32);
            auto status = service.addWorkContractGroup(&group);
            REQUIRE(status == WorkService::GroupOperationStatus::Added);
            REQUIRE(service.getWorkContractGroupCount() == 1);

            // Schedule a little work to ensure provider path is exercised
            auto h = group.createContract([]{});
            if (h.valid()) h.schedule();
        }
        // Group went out of scope; it should have notified the service and been removed.
        // The service must not crash and should show zero groups.
        REQUIRE(service.getWorkContractGroupCount() == 0);
        service.stop();
    }

    SECTION("Heap-allocated group: proper remove before delete releases retain") {
        WorkService service{WorkService::Config{}};
        service.start();
        auto* group = new WorkContractGroup(32);
        auto status = service.addWorkContractGroup(group);
        REQUIRE(status == WorkService::GroupOperationStatus::Added);
        REQUIRE(service.getWorkContractGroupCount() == 1);

        // Proper teardown path
        auto removed = service.removeWorkContractGroup(group);
        REQUIRE(removed == WorkService::GroupOperationStatus::Removed);
        REQUIRE(service.getWorkContractGroupCount() == 0);

        delete group; // Should be safe; service released its retain
        service.stop();
    }

    SECTION("Heap-allocated group: service.clear() releases retains") {
        WorkService service{WorkService::Config{}};
        service.start();
        auto* group = new WorkContractGroup(32);
        auto status = service.addWorkContractGroup(group);
        REQUIRE(status == WorkService::GroupOperationStatus::Added);
        REQUIRE(service.getWorkContractGroupCount() == 1);

        // Clear should release+disconnect all groups
        service.clear();
        REQUIRE(service.getWorkContractGroupCount() == 0);

        // Owner can safely delete afterwards
        delete group;
        service.stop();
    }

    SECTION("Heap-allocated group: delete without remove() auto-notifies service") {
        WorkService service{WorkService::Config{}};
        service.start();
        auto* group = new WorkContractGroup(32);
        auto status = service.addWorkContractGroup(group);
        REQUIRE(status == WorkService::GroupOperationStatus::Added);
        REQUIRE(service.getWorkContractGroupCount() == 1);

        // Delete without explicit remove. Group destructor should notify service, which
        // removes the group without calling release during destruction (to avoid double-deletes).
        delete group;

        // Service should have zero groups after notification
        REQUIRE(service.getWorkContractGroupCount() == 0);
        service.stop();
    }
}
