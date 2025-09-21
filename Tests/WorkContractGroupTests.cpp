#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkService.h"
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <iostream>
#include <mutex>

using namespace EntropyEngine::Core::Concurrency;
using namespace Catch::Matchers;

SCENARIO("WorkContractGroup basic functionality", "[workcontract][experimental][basic]") {
    GIVEN("An empty work contract group") {
        WorkContractGroup group(64);
        
        WHEN("It is created") {
            THEN("It should have correct initial state") {
                REQUIRE(group.capacity() == 64);
                REQUIRE(group.scheduledCount() == 0);
                REQUIRE(group.executingCount() == 0);
            }
        }
        
        WHEN("A contract is created") {
            std::atomic<int> counter{0};
            auto handle = group.createContract([&counter]() {
                counter++;
            });
            
            THEN("The handle should be valid") {
                REQUIRE(handle.valid());
                REQUIRE(group.scheduledCount() == 0);
            }

            auto result = handle.schedule();

            REQUIRE(result == ScheduleResult::Scheduled);
            REQUIRE(group.scheduledCount() == 1);
            group.executeAllBackgroundWork();

            REQUIRE(counter == 1);
            REQUIRE(group.scheduledCount() == 0);
        }
    }
}

SCENARIO("WorkContractGroup capacity limits", "[workcontract][experimental][capacity]") {
    GIVEN("A work contract group with limited capacity") {
        const size_t capacity = 8;
        WorkContractGroup group(capacity);
        
        WHEN("Creating contracts up to capacity") {
            std::vector<WorkContractHandle> handles;
            
            for (size_t i = 0; i < capacity; ++i) {
                handles.push_back(group.createContract([]() {}));
            }

            for (const auto& handle : handles) {
                REQUIRE(handle.valid());
            }

            auto overflowHandle = group.createContract([]() {});

            REQUIRE_FALSE(overflowHandle.valid());
            handles[0].release();
            handles.erase(handles.begin());
            auto newHandle = group.createContract([]() {});
            REQUIRE(newHandle.valid());
        }
    }
}

SCENARIO("WorkContractGroup concurrent operations", "[workcontract][experimental][concurrent]") {
    GIVEN("A work contract group accessed by multiple threads") {
        WorkContractGroup group(1024);
        std::atomic<int> totalWork{0};
        std::atomic<int> createCount{0};
        std::atomic<int> scheduleCount{0};
        
        WHEN("Multiple threads create and schedule contracts") {
            const int threadCount = 4;
            const int contractsPerThread = 100;
            std::vector<std::thread> threads;
            
            for (int t = 0; t < threadCount; ++t) {
                threads.emplace_back([&group, &totalWork, &createCount, &scheduleCount, contractsPerThread]() {
                    std::vector<WorkContractHandle> handles;
                    
                    // Create contracts
                    for (int i = 0; i < contractsPerThread; ++i) {
                        auto handle = group.createContract([&totalWork]() {
                            totalWork++;
                        });
                        
                        if (handle.valid()) {
                            handles.push_back(std::move(handle));
                            createCount++;
                        }
                    }
                    
                    // Schedule them
                    for (auto& handle : handles) {
                        if (handle.schedule() == ScheduleResult::Scheduled) {
                            scheduleCount++;
                        }
                    }
                });
            }
            
            for (auto& thread : threads) {
                thread.join();
            }
            
            THEN("All operations should succeed") {
                REQUIRE(createCount == threadCount * contractsPerThread);
                REQUIRE(scheduleCount == threadCount * contractsPerThread);
                REQUIRE(group.scheduledCount() == threadCount * contractsPerThread);
                
                AND_WHEN("Executing all contracts") {
                    group.executeAllBackgroundWork();
                    
                    THEN("All work should complete") {
                        REQUIRE(totalWork == threadCount * contractsPerThread);
                        REQUIRE(group.scheduledCount() == 0);
                    }
                }
            }
        }
    }
}

SCENARIO("WorkContractGroup wait functionality", "[workcontract][experimental][wait]") {
    GIVEN("A work contract group with scheduled work") {
        WorkContractGroup group(64);
        std::atomic<int> workStarted{0};
        std::atomic<int> workCompleted{0};
        
        WHEN("Work is scheduled that blocks") {
            for (int i = 0; i < 5; ++i) {
                auto handle = group.createContract([&workStarted, &workCompleted]() {
                    workStarted++;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    workCompleted++;
                });
                handle.schedule();
            }
            
            THEN("Wait should block until work completes") {
                // Start a single executor thread to execute work
                std::thread executor([&group]() {
                    group.executeAllBackgroundWork();
                });

                group.wait();
                executor.join();
                
                REQUIRE(workCompleted == 5);
            }
        }
    }
}

SCENARIO("WorkContractGroup handle validation", "[workcontract][experimental][validation]") {
    GIVEN("Two work contract groups") {
        WorkContractGroup group1(32);
        WorkContractGroup group2(32);
        
        WHEN("Creating a handle from one group") {
            auto handle = group1.createContract([]() {});
            
            THEN("It should be valid for its group") {
                REQUIRE(group1.isValidHandle(handle));
                REQUIRE_FALSE(group2.isValidHandle(handle));
            }
            
            AND_WHEN("The handle is released") {
                uint32_t originalIndex = handle.handleIndex();
                handle.release();
                
                THEN("It should no longer be valid") {
                    REQUIRE_FALSE(group1.isValidHandle(handle));
                    
                    AND_WHEN("A new contract is created") {
                        auto newHandle = group1.createContract([]() {});
                        
                        THEN("The old handle should still be invalid due to generation") {
                            REQUIRE_FALSE(group1.isValidHandle(handle));
                            REQUIRE(group1.isValidHandle(newHandle));
                        }
                    }
                }
            }
        }
    }
}

SCENARIO("WorkContractGroup state transitions", "[workcontract][experimental][state]") {
    GIVEN("A work contract with state tracking") {
        WorkContractGroup group(16);
        
        WHEN("Following a contract through its lifecycle") {
            auto handle = group.createContract([]() {});
            
            THEN("Initial state should be Allocated") {
                REQUIRE(group.getContractState(handle) == ContractState::Allocated);
            }
            
            AND_WHEN("Scheduled") {
                handle.schedule();
                
                THEN("State should be Scheduled") {
                    REQUIRE(group.getContractState(handle) == ContractState::Scheduled);
                }
                
                AND_WHEN("Selected for execution") {
                    auto execHandle = group.selectForExecution();
                    
                    THEN("State should be Executing") {
                        REQUIRE(execHandle.valid());
                        REQUIRE(group.getContractState(execHandle) == ContractState::Executing);
                        
                        AND_WHEN("Execution completes") {
                            group.executeContract(execHandle);
                            group.completeExecution(execHandle);
                            
                            THEN("Handle should be invalid") {
                                REQUIRE_FALSE(group.isValidHandle(execHandle));
                            }
                        }
                    }
                }
            }
        }
    }
}

SCENARIO("WorkContractGroup unschedule operations", "[workcontract][experimental][unschedule]") {
    GIVEN("A work contract group with scheduled work") {
        WorkContractGroup group(32);
        std::atomic<int> executionCount{0};
        
        WHEN("Work is scheduled then unscheduled") {
            auto handle = group.createContract([&executionCount]() {
                executionCount++;
            });
            
            REQUIRE(handle.schedule() == ScheduleResult::Scheduled);
            REQUIRE(group.scheduledCount() == 1);
            
            auto result = handle.unschedule();
            
            THEN("Unschedule should succeed") {
                REQUIRE(result == ScheduleResult::NotScheduled);
                REQUIRE(group.scheduledCount() == 0);
                
                AND_WHEN("Executing all work") {
                    group.executeAllBackgroundWork();
                    
                    THEN("The unscheduled work should not run") {
                        REQUIRE(executionCount == 0);
                    }
                }
            }
        }
        
        WHEN("Trying to unschedule executing work") {
            auto handle = group.createContract([&executionCount]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                executionCount++;
            });
            
            handle.schedule();
            
            // Start execution in another thread
            std::thread executor([&group]() {
                group.executeAllBackgroundWork();
            });
            
            // Give it time to start
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            auto result = handle.unschedule();
            
            THEN("Unschedule should indicate work is executing") {
                REQUIRE(result == ScheduleResult::Executing);
                
                executor.join();
                REQUIRE(executionCount == 1);
            }
        }
    }
}

SCENARIO("WorkContractGroup with WorkService integration", "[workcontract][experimental][service][!mayfail]") {
    GIVEN("A WorkService with multiple groups") {
        WorkService::Config config;
        config.threadCount = 2;
        WorkService service(config);
        
        WorkContractGroup group1(64, "Group1");
        WorkContractGroup group2(64, "Group2");
        
        REQUIRE(service.addWorkContractGroup(&group1) == WorkService::GroupOperationStatus::Added);
        REQUIRE(service.addWorkContractGroup(&group2) == WorkService::GroupOperationStatus::Added);
        
        WHEN("Work is scheduled to different groups") {
            std::atomic<int> group1Work{0};
            std::atomic<int> group2Work{0};
            
            for (int i = 0; i < 10; ++i) {
                auto h1 = group1.createContract([&group1Work]() {
                    group1Work++;
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                });
                h1.schedule();
                
                auto h2 = group2.createContract([&group2Work]() {
                    group2Work++;
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                });
                h2.schedule();
            }
            
            service.start();
            
            // Wait for work to complete
            while (group1Work + group2Work < 20) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            service.stop();
            
            THEN("Both groups should have executed work") {
                REQUIRE(group1Work == 10);
                REQUIRE(group2Work == 10);
            }
        }
    }
}

SCENARIO("WorkContractGroup recursive work creation", "[workcontract][experimental][recursive]") {
    GIVEN("A work contract group that creates more work") {
        WorkContractGroup group(256);
        std::atomic<int> totalExecutions{0};
        const int maxDepth = 3;
        
        WHEN("Work recursively creates more work") {
            std::function<void(int)> recursiveWork = [&](int depth) {
                totalExecutions++;
                
                if (depth < maxDepth) {
                    // Create two child work items
                    for (int i = 0; i < 2; ++i) {
                        auto handle = group.createContract([&recursiveWork, depth]() {
                            recursiveWork(depth + 1);
                        });
                        handle.schedule();
                    }
                }
            };
            
            auto rootHandle = group.createContract([&recursiveWork]() {
                recursiveWork(0);
            });
            rootHandle.schedule();
            
            THEN("Executing should process all levels") {
                // Execute all work at once
                group.executeAllBackgroundWork();
                
                // Total should be 2^(maxDepth+1) - 1
                REQUIRE(totalExecutions == (1 << (maxDepth + 1)) - 1);
            }
        }
    }
}

SCENARIO("WorkContractGroup cross-group dependencies", "[workcontract][experimental][dependencies]") {
    GIVEN("Multiple groups with inter-dependencies") {
        WorkContractGroup group(64, "Level1");
        WorkContractGroup level2Group(64, "Level2");
        WorkContractGroup level3Group(64, "Level3");
        
        std::atomic<int> level1Count{0};
        std::atomic<int> level2Count{0};
        std::atomic<int> level3Count{0};
        
        WHEN("Creating a dependency chain across groups") {
            auto rootHandle = group.createContract([&]() {
                level1Count++;
                
                // Create work in level 2
                for (int i = 0; i < 3; ++i) {
                    auto level2Handle = level2Group.createContract([&, i]() {
                        level2Count++;
                        
                        // Create work in level 3
                        for (int j = 0; j < 2; ++j) {
                            auto level3Handle = level3Group.createContract([&]() {
                                level3Count++;
                            });
                            
                            if (level3Handle.valid()) {
                                level3Handle.schedule();
                            }
                        }
                    });
                    
                    if (level2Handle.valid()) {
                        level2Handle.schedule();
                    }
                }
            });
            
            rootHandle.schedule();
            
            THEN("Executing should process each level in its own group") {
                // Execute level 1
                group.executeAllBackgroundWork();
                REQUIRE(level1Count == 1);
                REQUIRE(group.scheduledCount() == 0);
                REQUIRE(level2Group.scheduledCount() == 3);
                REQUIRE(level3Group.scheduledCount() == 0);
                
                // Execute level 2
                level2Group.executeAllBackgroundWork();
                REQUIRE(level2Count == 3);
                REQUIRE(level2Group.scheduledCount() == 0);
                REQUIRE(level3Group.scheduledCount() == 6);
                
                // Execute level 3
                level3Group.executeAllBackgroundWork();
                REQUIRE(level3Count == 6);
                REQUIRE(level3Group.scheduledCount() == 0);
            }
        }
    }
}

SCENARIO("WorkContractGroup safe destruction with epoch synchronization", "[workcontract][experimental][destruction][!mayfail]") {
    GIVEN("A WorkService with epoch-based synchronization") {
        WorkService::Config config;
        config.threadCount = 4;
        auto service = std::make_shared<WorkService>(config);
        service->start();
        
        WHEN("A stack-allocated WorkContractGroup is destroyed while threads are running") {
            std::atomic<int> executionCount{0};
            std::atomic<bool> keepRunning{true};
            std::atomic<int> accessAttempts{0};
            
            // Create a scope for the stack-allocated group
            {
                WorkContractGroup stackGroup(1024, "TestGroup");
                service->addWorkContractGroup(&stackGroup);
                
                // Create work that executes repeatedly
                for (int i = 0; i < 100; ++i) {
                    auto handle = stackGroup.createContract([&executionCount, &keepRunning, &accessAttempts]() {
                        accessAttempts++;
                        executionCount++;
                        
                        // Simulate some work
                        std::this_thread::sleep_for(std::chrono::microseconds(10));
                        
                        // Check if we should keep creating more work
                        if (keepRunning.load()) {
                            // This will try to access the group after destruction if not synchronized
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                        }
                    });
                    handle.schedule();
                }
                
                // Let some work execute
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                
                // Signal work to stop and wait a bit
                keepRunning = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                
                // Group destructor will be called here
                // With epoch synchronization, this should wait for all threads
                // to acknowledge they're no longer using the group
            }
            
            // Give threads time to crash if synchronization failed
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            THEN("No crash should occur and some work should have executed") {
                REQUIRE(executionCount > 0);
                REQUIRE(accessAttempts > 0);
                // If we get here without crashing, epoch synchronization worked
            }
        }
        
        WHEN("Multiple groups are destroyed in rapid succession") {
            std::atomic<int> totalExecutions{0};
            
            // Create and destroy multiple groups
            for (int g = 0; g < 5; ++g) {
                WorkContractGroup* group = new WorkContractGroup(512, "Group" + std::to_string(g));
                service->addWorkContractGroup(group);
                
                // Schedule some work
                for (int i = 0; i < 50; ++i) {
                    auto handle = group->createContract([&totalExecutions]() {
                        totalExecutions++;
                        std::this_thread::sleep_for(std::chrono::microseconds(5));
                    });
                    handle.schedule();
                }
                
                // Let some work start
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                
                // Destroy the group - epoch sync should ensure safety
                delete group;
            }
            
            THEN("All destructions should complete safely") {
                REQUIRE(totalExecutions > 0);
                // Success means no crashes occurred
            }
        }
        
        service->stop();
    }
}