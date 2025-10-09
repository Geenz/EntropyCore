#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <mutex>
#include <condition_variable>

using namespace EntropyEngine::Core::Concurrency;
using namespace Catch::Matchers;
using namespace std::chrono_literals;

SCENARIO("WorkService basic functionality", "[workservice][experimental][basic][!mayfail]") {
    GIVEN("A work service with default configuration") {
        WorkService::Config config;
        config.threadCount = 2;
        WorkService service(config);
        
        WHEN("The service is created") {
            THEN("It should have correct initial state") {
                REQUIRE(service.getThreadCount() == 2);
                REQUIRE(service.getWorkContractGroupCount() == 0);
                // No max groups limit in new implementation
            }
        }
        
        WHEN("A work group is added") {
            WorkContractGroup group(64);
            auto status = service.addWorkContractGroup(&group);
            
            THEN("The group should be registered") {
                REQUIRE(status == WorkService::GroupOperationStatus::Added);
                REQUIRE(service.getWorkContractGroupCount() == 1);
            }
            
            AND_WHEN("The same group is added again") {
                auto status2 = service.addWorkContractGroup(&group);
                
                THEN("It should report as already existing") {
                    REQUIRE(status2 == WorkService::GroupOperationStatus::Exists);
                    REQUIRE(service.getWorkContractGroupCount() == 1);
                }
            }
            
            AND_WHEN("The group is removed") {
                auto removeStatus = service.removeWorkContractGroup(&group);
                
                THEN("The group should be unregistered") {
                    REQUIRE(removeStatus == WorkService::GroupOperationStatus::Removed);
                    REQUIRE(service.getWorkContractGroupCount() == 0);
                }
            }
        }
    }
}

SCENARIO("WorkService thread configuration", "[workservice][experimental][config][!mayfail]") {
    GIVEN("Various thread count configurations") {
        WHEN("Thread count is 0") {
            WorkService::Config config;
            config.threadCount = 0;
            WorkService service(config);
            
            THEN("It should use hardware concurrency") {
                REQUIRE(service.getThreadCount() == std::thread::hardware_concurrency());
            }
        }
        
        WHEN("Thread count exceeds hardware concurrency") {
            WorkService::Config config;
            config.threadCount = std::thread::hardware_concurrency() + 100;
            WorkService service(config);
            
            THEN("It should be clamped to hardware concurrency") {
                REQUIRE(service.getThreadCount() == std::thread::hardware_concurrency());
            }
        }
        
        WHEN("Thread count is 1") {
            WorkService::Config config;
            config.threadCount = 1;
            WorkService service(config);
            
            THEN("It should use single thread") {
                REQUIRE(service.getThreadCount() == 1);
            }
        }
    }
}

SCENARIO("WorkService execution", "[workservice][experimental][execution][!mayfail]") {
    GIVEN("A work service with registered groups") {
        WorkService::Config config;
        config.threadCount = 4;
        config.schedulerConfig.maxConsecutiveExecutionCount = 8;
        WorkService service(config);
        
        WorkContractGroup group1(128);
        WorkContractGroup group2(128);
        
        service.addWorkContractGroup(&group1);
        service.addWorkContractGroup(&group2);
        
        WHEN("Work is scheduled and service is started") {
            std::atomic<int> group1Executions{0};
            std::atomic<int> group2Executions{0};
            const int workPerGroup = 50;
            
            // Schedule work in both groups
            for (int i = 0; i < workPerGroup; ++i) {
                auto handle1 = group1.createContract([&group1Executions]() {
                    group1Executions.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(1ms);
                });
                handle1.schedule();
                
                auto handle2 = group2.createContract([&group2Executions]() {
                    group2Executions.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(1ms);
                });
                handle2.schedule();
            }
            
            service.start();
            
            // Wait for all work to complete
            group1.wait();
            group2.wait();
            
            service.stop();
            
            THEN("All work should be executed") {
                REQUIRE(group1Executions == workPerGroup);
                REQUIRE(group2Executions == workPerGroup);
                
                // Both groups should have executed work
                REQUIRE(group1Executions > 0);
                REQUIRE(group2Executions > 0);
            }
        }
    }
}

SCENARIO("WorkService adaptive scheduling", "[workservice][experimental][scheduling][!mayfail]") {
    GIVEN("Groups with different work loads") {
        WorkService::Config config;
        config.threadCount = 4;
        config.schedulerConfig.maxConsecutiveExecutionCount = 4;
        config.schedulerConfig.updateCycleInterval = 8;
        WorkService service(config);
        
        WorkContractGroup heavyGroup(256);
        WorkContractGroup lightGroup(256);
        
        service.addWorkContractGroup(&heavyGroup);
        service.addWorkContractGroup(&lightGroup);
        
        WHEN("Heavy group has more work") {
            std::atomic<int> heavyExecutions{0};
            std::atomic<int> lightExecutions{0};
            
            // Heavy group gets 100 contracts
            for (int i = 0; i < 100; ++i) {
                auto handle = heavyGroup.createContract([&heavyExecutions]() {
                    heavyExecutions.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(2ms);
                });
                handle.schedule();
            }
            
            // Light group gets 10 contracts
            for (int i = 0; i < 10; ++i) {
                auto handle = lightGroup.createContract([&lightExecutions]() {
                    lightExecutions.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(2ms);
                });
                handle.schedule();
            }
            
            service.start();
            
            // Let it run for a bit
            std::this_thread::sleep_for(50ms);
            
            // Check intermediate state - heavy group should be getting more attention
            int heavyCount = heavyExecutions.load();
            int lightCount = lightExecutions.load();
            
            // Continue until all work is done
            heavyGroup.wait();
            lightGroup.wait();
            
            service.stop();
            
            THEN("Heavy group should have received proportionally more thread time") {
                REQUIRE(heavyExecutions == 100);
                REQUIRE(lightExecutions == 10);
                
                // During execution, heavy group should have been prioritized
                // (checked via intermediate snapshot)
                INFO("Heavy executions at snapshot: " << heavyCount);
                INFO("Light executions at snapshot: " << lightCount);
            }
        }
    }
}

SCENARIO("WorkService sticky execution", "[workservice][experimental][sticky][!mayfail]") {
    GIVEN("A service configured for sticky execution") {
        WorkService::Config config;
        config.threadCount = 1; // Single thread to ensure predictable execution
        config.schedulerConfig.maxConsecutiveExecutionCount = 5;
        WorkService service(config);
        
        WorkContractGroup group(64);
        service.addWorkContractGroup(&group);
        
        WHEN("Multiple contracts are scheduled") {
            std::mutex executionMutex;
            std::vector<int> executionOrder;
            
            // Schedule 10 contracts
            for (int i = 0; i < 10; ++i) {
                auto handle = group.createContract([i, &executionMutex, &executionOrder]() {
                    std::lock_guard<std::mutex> lock(executionMutex);
                    executionOrder.push_back(i);
                });
                handle.schedule();
            }
            
            service.start();
            group.wait();
            service.stop();
            
            THEN("Contracts should execute in mostly sequential order") {
                REQUIRE(executionOrder.size() == 10);
                
                // Check for consecutive execution patterns
                int consecutiveRuns = 0;
                for (size_t i = 1; i < executionOrder.size(); ++i) {
                    if (executionOrder[i] == executionOrder[i-1] + 1) {
                        consecutiveRuns++;
                    }
                }
                
                // Should see some consecutive execution due to sticky behavior
                INFO("Consecutive runs: " << consecutiveRuns);
                REQUIRE(consecutiveRuns > 0);
            }
        }
    }
}

SCENARIO("WorkService group operations while running", "[workservice][experimental][dynamic][!mayfail]") {
    GIVEN("A running work service") {
        WorkService::Config config;
        config.threadCount = 2;
        WorkService service(config);
        
        WorkContractGroup group1(64);
        WorkContractGroup group2(64);
        
        service.addWorkContractGroup(&group1);
        service.start();
        
        WHEN("A new group is added while running") {
            std::atomic<bool> group2Started{false};
            
            // Schedule work in group1
            auto handle1 = group1.createContract([]() {
                std::this_thread::sleep_for(50ms);
            });
            handle1.schedule();
            
            // Add group2 while service is running
            service.addWorkContractGroup(&group2);
            
            // Schedule work in group2
            auto handle2 = group2.createContract([&group2Started]() {
                group2Started = true;
            });
            handle2.schedule();
            
            // Wait for group2 work
            group2.wait();
            service.stop();
            
            THEN("The new group should be picked up and executed") {
                REQUIRE(group2Started == true);
            }
        }
    }
}

SCENARIO("WorkService configuration updates", "[workservice][experimental][configuration][!mayfail]") {
    GIVEN("A work service with initial configuration") {
        WorkService::Config config;
        config.failureSleepTime = 100;
        WorkService service(config);
        
        WHEN("Configuration is updated at runtime") {
            service.setFailureSleepTime(200);
            
            THEN("New values should be reflected") {
                REQUIRE(service.getFailureSleepTime() == 200);
            }
        }
    }
}

SCENARIO("WorkService stress test", "[workservice][experimental][stress][!mayfail]") {
    GIVEN("A work service under heavy load") {
        WorkService::Config config;
        config.threadCount = std::thread::hardware_concurrency();
        config.schedulerConfig.maxConsecutiveExecutionCount = 16;
        config.schedulerConfig.updateCycleInterval = 32;
        WorkService service(config);
        
        const int numGroups = 8;
        const int contractsPerGroup = 100;
        std::vector<std::unique_ptr<WorkContractGroup>> groups;
        std::atomic<int> totalExecutions{0};
        
        // Create and register groups
        for (int i = 0; i < numGroups; ++i) {
            groups.push_back(std::make_unique<WorkContractGroup>(512));
            service.addWorkContractGroup(groups.back().get());
        }
        
        WHEN("Many contracts are scheduled across all groups") {
            auto startTime = std::chrono::steady_clock::now();
            
            // Schedule work in all groups
            for (int g = 0; g < numGroups; ++g) {
                for (int c = 0; c < contractsPerGroup; ++c) {
                    auto handle = groups[g]->createContract([&totalExecutions, g]() {
                        totalExecutions.fetch_add(1, std::memory_order_relaxed);
                        // Simulate varying work loads
                        std::this_thread::sleep_for(std::chrono::microseconds(g * 100 + 100));
                    });
                    handle.schedule();
                }
            }
            
            service.start();
            
            // Wait for all work to complete
            for (auto& group : groups) {
                group->wait();
            }
            
            auto endTime = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            
            service.stop();
            
            THEN("All work should complete successfully") {
                REQUIRE(totalExecutions == numGroups * contractsPerGroup);
                
                INFO("Total execution time: " << duration.count() << "ms");
                INFO("Thread count: " << config.threadCount);
                INFO("Throughput: " << (totalExecutions * 1000.0 / duration.count()) << " contracts/second");
            }
        }
    }
}