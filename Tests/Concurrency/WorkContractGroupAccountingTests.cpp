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
#include "../src/Concurrency/SignalTree.h"
#include <thread>
#include <atomic>
#include <vector>
#include <random>
#include <condition_variable>
#include <mutex>

using namespace EntropyEngine::Core::Concurrency;

/**
 * Helper class to validate accounting invariants
 */
class AccountingValidator {
public:
    struct AccountingSnapshot {
        size_t activeCount;
        size_t scheduledCount;
        size_t executingCount;
        size_t mainThreadScheduledCount;
        size_t mainThreadExecutingCount;
        
        AccountingSnapshot(const WorkContractGroup& group) :
            activeCount(group.activeCount()),
            scheduledCount(group.scheduledCount()),
            executingCount(group.executingCount()),
            mainThreadScheduledCount(group.mainThreadScheduledCount()),
            mainThreadExecutingCount(group.mainThreadExecutingCount())
        {}
    };
    
    static bool verifyInvariants(const WorkContractGroup& group) {
        auto snapshot = AccountingSnapshot(group);
        
        // Invariant 1: scheduled + executing <= active
        if (snapshot.scheduledCount + snapshot.executingCount > snapshot.activeCount) {
            INFO("Failed: scheduled(" << snapshot.scheduledCount 
                 << ") + executing(" << snapshot.executingCount 
                 << ") > active(" << snapshot.activeCount << ")");
            return false;
        }
        
        // Invariant 2: mainThreadScheduled + mainThreadExecuting <= active
        if (snapshot.mainThreadScheduledCount + snapshot.mainThreadExecutingCount > snapshot.activeCount) {
            INFO("Failed: mainThreadScheduled(" << snapshot.mainThreadScheduledCount 
                 << ") + mainThreadExecuting(" << snapshot.mainThreadExecutingCount 
                 << ") > active(" << snapshot.activeCount << ")");
            return false;
        }
        
        // Invariant 3: active <= capacity
        if (snapshot.activeCount > group.capacity()) {
            INFO("Failed: active(" << snapshot.activeCount 
                 << ") > capacity(" << group.capacity() << ")");
            return false;
        }
        
        return true;
    }
    
    static bool verifyTransition(const AccountingSnapshot& before, 
                                const AccountingSnapshot& after,
                                const std::string& operation) {
        // Track what should change for each operation
        if (operation == "create") {
            if (after.activeCount != before.activeCount + 1) {
                INFO("Create: active count should increase by 1");
                return false;
            }
        }
        else if (operation == "schedule") {
            if (after.scheduledCount != before.scheduledCount + 1) {
                INFO("Schedule: scheduled count should increase by 1");
                return false;
            }
        }
        else if (operation == "schedule_main") {
            if (after.mainThreadScheduledCount != before.mainThreadScheduledCount + 1) {
                INFO("Schedule main: main thread scheduled count should increase by 1");
                return false;
            }
        }
        else if (operation == "select") {
            if (after.scheduledCount != before.scheduledCount - 1 ||
                after.executingCount != before.executingCount + 1) {
                INFO("Select: scheduled should decrease, executing should increase");
                return false;
            }
        }
        else if (operation == "complete") {
            if (after.executingCount != before.executingCount - 1 ||
                after.activeCount != before.activeCount - 1) {
                INFO("Complete: executing and active should decrease by 1");
                return false;
            }
        }
        
        return true;
    }
};

TEST_CASE("WorkContractGroup accounting invariants", "[workcontract][accounting]") {
    SECTION("Basic lifecycle accounting") {
        WorkContractGroup group(10);
        
        REQUIRE(AccountingValidator::verifyInvariants(group));
        auto snapshot1 = AccountingValidator::AccountingSnapshot(group);
        
        // Create
        auto handle = group.createContract([]() {});
        REQUIRE(AccountingValidator::verifyInvariants(group));
        auto snapshot2 = AccountingValidator::AccountingSnapshot(group);
        REQUIRE(AccountingValidator::verifyTransition(snapshot1, snapshot2, "create"));
        
        // Schedule
        handle.schedule();
        REQUIRE(AccountingValidator::verifyInvariants(group));
        auto snapshot3 = AccountingValidator::AccountingSnapshot(group);
        REQUIRE(AccountingValidator::verifyTransition(snapshot2, snapshot3, "schedule"));
        
        // Select for execution
        auto execHandle = group.selectForExecution();
        REQUIRE(execHandle.valid());
        REQUIRE(AccountingValidator::verifyInvariants(group));
        auto snapshot4 = AccountingValidator::AccountingSnapshot(group);
        REQUIRE(AccountingValidator::verifyTransition(snapshot3, snapshot4, "select"));
        
        // Complete execution
        group.executeContract(execHandle);
        group.completeExecution(execHandle);
        REQUIRE(AccountingValidator::verifyInvariants(group));
        auto snapshot5 = AccountingValidator::AccountingSnapshot(group);
        REQUIRE(AccountingValidator::verifyTransition(snapshot4, snapshot5, "complete"));
    }
    
    SECTION("Main thread accounting separation") {
        WorkContractGroup group(10);
        
        // Create and schedule regular work
        auto regular = group.createContract([]() {}, ExecutionType::AnyThread);
        regular.schedule();
        
        REQUIRE(group.scheduledCount() == 1);
        REQUIRE(group.mainThreadScheduledCount() == 0);
        
        // Create and schedule main thread work
        auto mainThread = group.createContract([]() {}, ExecutionType::MainThread);
        mainThread.schedule();
        
        REQUIRE(group.scheduledCount() == 1);  // Regular unchanged
        REQUIRE(group.mainThreadScheduledCount() == 1);
        
        // Verify they use separate queues
        auto execRegular = group.selectForExecution();
        REQUIRE(execRegular.valid());
        REQUIRE(group.scheduledCount() == 0);
        REQUIRE(group.mainThreadScheduledCount() == 1);  // Main thread unchanged
        
        auto execMain = group.selectForMainThreadExecution();
        REQUIRE(execMain.valid());
        REQUIRE(group.mainThreadScheduledCount() == 0);
        
        // Clean up
        group.executeContract(execRegular);
        group.completeExecution(execRegular);
        group.executeContract(execMain);
        group.completeMainThreadExecution(execMain);
        
        REQUIRE(AccountingValidator::verifyInvariants(group));
    }
    
    SECTION("Accounting under concurrent operations") {
        WorkContractGroup group(1024);
        const int numThreads = 8;
        const int opsPerThread = 100;
        std::atomic<bool> startFlag{false};
        std::atomic<int> errors{0};
        
        std::vector<std::thread> threads;
        
        for (int t = 0; t < numThreads; ++t) {
            threads.emplace_back([&group, &startFlag, &errors, opsPerThread, t]() {
                std::mt19937 rng(t);
                std::uniform_int_distribution<int> opDist(0, 3);
                std::vector<WorkContractHandle> handles;
                
                // Wait for start
                while (!startFlag.load()) {
                    std::this_thread::yield();
                }
                
                for (int op = 0; op < opsPerThread; ++op) {
                    // Verify invariants hold
                    if (!AccountingValidator::verifyInvariants(group)) {
                        errors.fetch_add(1);
                    }
                    
                    int operation = opDist(rng);
                    
                    switch (operation) {
                        case 0: {  // Create and schedule
                            auto handle = group.createContract([]() {});
                            if (handle.valid()) {
                                handle.schedule();
                                handles.push_back(handle);
                            }
                            break;
                        }
                        case 1: {  // Select and execute
                            auto handle = group.selectForExecution();
                            if (handle.valid()) {
                                group.executeContract(handle);
                                group.completeExecution(handle);
                            }
                            break;
                        }
                        case 2: {  // Unschedule
                            if (!handles.empty()) {
                                auto& handle = handles.back();
                                handle.unschedule();
                                handles.pop_back();
                            }
                            break;
                        }
                        case 3: {  // Release
                            if (!handles.empty()) {
                                handles.back().release();
                                handles.pop_back();
                            }
                            break;
                        }
                    }
                }
                
                // Clean up remaining handles
                for (auto& handle : handles) {
                    handle.release();
                }
            });
        }
        
        startFlag = true;
        
        for (auto& t : threads) {
            t.join();
        }
        
        REQUIRE(errors.load() == 0);
        REQUIRE(AccountingValidator::verifyInvariants(group));
    }
    
    SECTION("Accounting accuracy during wait") {
        WorkContractGroup group(10);
        std::atomic<int> executionCount{0};
        std::atomic<int> workStarted{0};
        std::mutex executionMutex;
        std::condition_variable executionCV;
        
        // Schedule several contracts that signal when they start
        for (int i = 0; i < 5; ++i) {
            auto handle = group.createContract([&]() {
                workStarted.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lock(executionMutex);
                    executionCV.notify_all();
                }
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                executionCount.fetch_add(1);
            });
            handle.schedule();
        }
        
        REQUIRE(group.scheduledCount() == 5);
        REQUIRE(group.executingCount() == 0);
        REQUIRE(AccountingValidator::verifyInvariants(group));
        
        // Start a single executor thread that processes all work
        std::thread executor([&group]() {
            while (true) {
                auto handle = group.selectForExecution();
                if (!handle.valid()) {
                    break;
                }
                group.executeContract(handle);
                group.completeExecution(handle);
            }
        });
        
        // Wait for all work to be picked up and started
        {
            std::unique_lock<std::mutex> lock(executionMutex);
            executionCV.wait(lock, [&workStarted]() {
                return workStarted.load() >= 5;
            });
        }
        
        // Now all work has been executed
        executor.join();
        
        // Verify final state
        REQUIRE(executionCount.load() == 5);
        REQUIRE(group.scheduledCount() == 0);
        REQUIRE(group.executingCount() == 0);
        REQUIRE(group.activeCount() == 0);
        REQUIRE(AccountingValidator::verifyInvariants(group));
        
        // wait() should return immediately since everything is done
        group.wait();
    }
    
    SECTION("Capacity boundary accounting") {
        const size_t capacity = 8;
        WorkContractGroup group(capacity);
        
        std::vector<WorkContractHandle> handles;
        
        // Fill to capacity
        for (size_t i = 0; i < capacity; ++i) {
            auto handle = group.createContract([]() {});
            REQUIRE(handle.valid());
            handles.push_back(handle);
        }
        
        REQUIRE(group.activeCount() == capacity);
        
        // Verify overflow doesn't corrupt accounting
        auto overflow = group.createContract([]() {});
        REQUIRE(!overflow.valid());
        REQUIRE(group.activeCount() == capacity);  // Should not change
        
        // Release one and verify accounting
        handles[0].release();
        handles.erase(handles.begin());
        REQUIRE(group.activeCount() == capacity - 1);
        
        // Should be able to create one more
        auto newHandle = group.createContract([]() {});
        REQUIRE(newHandle.valid());
        REQUIRE(group.activeCount() == capacity);
        
        // Clean up
        newHandle.release();
        for (auto& h : handles) {
            h.release();
        }
        
        REQUIRE(group.activeCount() == 0);
    }
    
    SECTION("Recursive work accounting") {
        WorkContractGroup group(256);
        std::atomic<int> totalCreated{0};
        std::atomic<int> totalExecuted{0};
        
        std::function<void(int)> recursiveWork = [&](int depth) {
            totalExecuted.fetch_add(1);
            
            if (depth > 0) {
                for (int i = 0; i < 2; ++i) {
                    auto handle = group.createContract([&recursiveWork, depth]() {
                        recursiveWork(depth - 1);
                    });
                    
                    if (handle.valid()) {
                        totalCreated.fetch_add(1);
                        handle.schedule();
                    }
                }
            }
        };
        
        // Start recursive work
        auto root = group.createContract([&recursiveWork]() {
            recursiveWork(3);
        });
        root.schedule();
        totalCreated = 1;
        
        // Execute with accounting checks
        while (group.scheduledCount() > 0) {
            REQUIRE(AccountingValidator::verifyInvariants(group));
            
            auto handle = group.selectForExecution();
            if (handle.valid()) {
                group.executeContract(handle);
                group.completeExecution(handle);
            }
        }
        
        REQUIRE(totalExecuted.load() == totalCreated.load());
        REQUIRE(group.activeCount() == 0);
        REQUIRE(group.scheduledCount() == 0);
        REQUIRE(group.executingCount() == 0);
    }
}

TEST_CASE("WorkContractGroup SignalTree consistency", "[workcontract][signaltree][accounting]") {
    SECTION("Scheduled count matches SignalTree state") {
        WorkContractGroup group(64);
        
        // Schedule multiple contracts
        std::vector<WorkContractHandle> handles;
        for (int i = 0; i < 10; ++i) {
            auto handle = group.createContract([]() {});
            handle.schedule();
            handles.push_back(handle);
        }
        
        // The scheduled count should match what SignalTree would report
        // (We can't directly access the SignalTree, but we can verify consistency)
        REQUIRE(group.scheduledCount() == 10);
        
        // Select half
        for (int i = 0; i < 5; ++i) {
            auto handle = group.selectForExecution();
            REQUIRE(handle.valid());
            group.executeContract(handle);
            group.completeExecution(handle);
        }
        
        REQUIRE(group.scheduledCount() == 5);
        
        // Unschedule some
        for (int i = 0; i < 3; ++i) {
            handles[i].unschedule();
        }
        
        // Note: Some may have already been executed, so we can't assert exact count
        // But we can verify invariants still hold
        REQUIRE(AccountingValidator::verifyInvariants(group));
    }
    
    SECTION("Main thread SignalTree independence") {
        WorkContractGroup group(64);
        
        // Schedule to both queues
        for (int i = 0; i < 5; ++i) {
            auto regular = group.createContract([]() {}, ExecutionType::AnyThread);
            regular.schedule();
            
            auto mainThread = group.createContract([]() {}, ExecutionType::MainThread);
            mainThread.schedule();
        }
        
        REQUIRE(group.scheduledCount() == 5);
        REQUIRE(group.mainThreadScheduledCount() == 5);
        
        // Execute all regular work
        group.executeAllBackgroundWork();
        
        REQUIRE(group.scheduledCount() == 0);
        REQUIRE(group.mainThreadScheduledCount() == 5);  // Should be unchanged
        
        // Execute all main thread work
        size_t executed = group.executeAllMainThreadWork();
        
        REQUIRE(executed == 5);
        REQUIRE(group.mainThreadScheduledCount() == 0);
    }
    
    SECTION("Empty state consistency") {
        WorkContractGroup group(32);
        
        // Initially empty
        REQUIRE(group.scheduledCount() == 0);
        REQUIRE(group.hasMainThreadWork() == false);
        
        // Schedule and unschedule
        auto handle = group.createContract([]() {});
        handle.schedule();
        REQUIRE(group.scheduledCount() == 1);
        
        handle.unschedule();
        REQUIRE(group.scheduledCount() == 0);
        
        // Schedule to main thread
        auto mainHandle = group.createContract([]() {}, ExecutionType::MainThread);
        mainHandle.schedule();
        REQUIRE(group.hasMainThreadWork() == true);
        
        mainHandle.unschedule();
        REQUIRE(group.hasMainThreadWork() == false);
    }
}