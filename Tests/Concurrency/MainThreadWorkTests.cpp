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
#include "../src/Concurrency/WorkService.h"
#include <atomic>
#include <thread>

using namespace EntropyEngine::Core::Concurrency;

TEST_CASE("Main thread work execution", "[workcontract][mainthread]") {
    SECTION("Basic main thread contract execution") {
        WorkContractGroup group(10);
        std::atomic<bool> executed{false};
        
        // Create a main thread targeted contract
        auto handle = group.createContract(
            [&executed]() { executed = true; },
            ExecutionType::MainThread
        );
        
        REQUIRE(handle.valid());
        REQUIRE(group.mainThreadScheduledCount() == 0);
        
        // Schedule it
        auto result = handle.schedule();
        REQUIRE(result == ScheduleResult::Scheduled);
        REQUIRE(group.mainThreadScheduledCount() == 1);
        REQUIRE(group.scheduledCount() == 0); // Should not be in regular queue
        
        // Execute main thread work
        size_t count = group.executeAllMainThreadWork();
        REQUIRE(count == 1);
        REQUIRE(executed == true);
        REQUIRE(group.mainThreadScheduledCount() == 0);
    }
    
    SECTION("Mixed execution types") {
        WorkContractGroup group(10);
        std::atomic<int> regularCount{0};
        std::atomic<int> mainThreadCount{0};
        
        // Create regular contracts
        for (int i = 0; i < 3; ++i) {
            auto handle = group.createContract(
                [&regularCount]() { regularCount++; },
                ExecutionType::AnyThread
            );
            handle.schedule();
        }
        
        // Create main thread contracts
        for (int i = 0; i < 2; ++i) {
            auto handle = group.createContract(
                [&mainThreadCount]() { mainThreadCount++; },
                ExecutionType::MainThread
            );
            handle.schedule();
        }
        
        REQUIRE(group.scheduledCount() == 3);
        REQUIRE(group.mainThreadScheduledCount() == 2);
        
        // Execute regular work
        group.executeAllBackgroundWork();
        REQUIRE(regularCount == 3);
        REQUIRE(mainThreadCount == 0); // Should not execute main thread work
        
        // Execute main thread work
        size_t count = group.executeAllMainThreadWork();
        REQUIRE(count == 2);
        REQUIRE(mainThreadCount == 2);
    }
    
    SECTION("WorkService main thread execution") {
        WorkService::Config config;
        config.threadCount = 2;
        WorkService service(config);
        
        WorkContractGroup group1(10);
        WorkContractGroup group2(10);
        
        service.addWorkContractGroup(&group1);
        service.addWorkContractGroup(&group2);
        
        std::atomic<int> group1Count{0};
        std::atomic<int> group2Count{0};
        
        // Add main thread work to both groups
        for (int i = 0; i < 3; ++i) {
            auto handle = group1.createContract(
                [&group1Count]() { group1Count++; },
                ExecutionType::MainThread
            );
            handle.schedule();
        }
        
        for (int i = 0; i < 2; ++i) {
            auto handle = group2.createContract(
                [&group2Count]() { group2Count++; },
                ExecutionType::MainThread
            );
            handle.schedule();
        }
        
        REQUIRE(service.hasMainThreadWork() == true);
        
        // Execute all main thread work
        auto result = service.executeMainThreadWork();
        REQUIRE(result.contractsExecuted == 5);
        REQUIRE(result.groupsWithWork == 2);
        REQUIRE(result.moreWorkAvailable == false);
        REQUIRE(group1Count == 3);
        REQUIRE(group2Count == 2);
        
        REQUIRE(service.hasMainThreadWork() == false);
        
        service.removeWorkContractGroup(&group1);
        service.removeWorkContractGroup(&group2);
    }
    
    SECTION("Limited main thread execution") {
        WorkContractGroup group(10);
        std::atomic<int> counter{0};
        
        // Create 5 main thread contracts
        for (int i = 0; i < 5; ++i) {
            auto handle = group.createContract(
                [&counter]() { counter++; },
                ExecutionType::MainThread
            );
            handle.schedule();
        }
        
        REQUIRE(group.mainThreadScheduledCount() == 5);
        
        // Execute only 3
        size_t count = group.executeMainThreadWork(3);
        REQUIRE(count == 3);
        REQUIRE(counter == 3);
        REQUIRE(group.mainThreadScheduledCount() == 2);
        REQUIRE(group.hasMainThreadWork() == true);
        
        // Execute the rest
        count = group.executeAllMainThreadWork();
        REQUIRE(count == 2);
        REQUIRE(counter == 5);
        REQUIRE(group.mainThreadScheduledCount() == 0);
        REQUIRE(group.hasMainThreadWork() == false);
    }
}