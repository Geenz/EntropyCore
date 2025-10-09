#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "Concurrency/WorkContractGroup.h"
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <iostream>

using namespace EntropyEngine::Core::Concurrency;

TEST_CASE("WorkContractGroup High Contention Benchmark", "[benchmark][highcontention][workcontract]") {
    const size_t groupCapacity = 1024;
    const size_t contractsPerThread = 100;
    const int numThreads = 16;
    
    SECTION("Debug High Contention") {
        INFO("Running debug test before benchmark");
        
        // First run a simple test to verify it works
        {
            WorkContractGroup testGroup(256);
            std::atomic<int> count{0};
            
            auto h1 = testGroup.createContract([&count]() { count++; });
            auto h2 = testGroup.createContract([&count]() { count++; });
            
            REQUIRE(h1.valid());
            REQUIRE(h2.valid());
            
            h1.release();
            h2.release();
            
            INFO("Simple test passed");
        }
        
        // Now test with threads
        {
            WorkContractGroup testGroup(256);
            std::atomic<int> count{0};
            
            std::thread t1([&testGroup, &count]() {
                for (int i = 0; i < 10; ++i) {
                    auto h = testGroup.createContract([]() {});
                    if (h.valid()) {
                        count++;
                        h.release();
                    }
                }
            });
            
            std::thread t2([&testGroup, &count]() {
                for (int i = 0; i < 10; ++i) {
                    auto h = testGroup.createContract([]() {});
                    if (h.valid()) {
                        count++;
                        h.release();
                    }
                }
            });
            
            t1.join();
            t2.join();
            
            INFO("Thread test passed with count: " << count.load());
        }
    }
    
    SECTION("Simple High Contention Test") {
        // Create a group on the stack to avoid shared_ptr issues
        WorkContractGroup testGroup(groupCapacity);
        
        std::atomic<int> successCount{0};
        std::atomic<bool> startFlag{false};
        
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        
        for (int t = 0; t < numThreads; ++t) {
            threads.emplace_back([&testGroup, &successCount, &startFlag, contractsPerThread]() {
                // Wait for all threads to be ready
                while (!startFlag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                
                // All threads try to allocate at the same time
                for (size_t i = 0; i < contractsPerThread; ++i) {
                    // Create a simple lambda that doesn't capture anything
                    auto handle = testGroup.createContract([]() { 
                        // Empty work function
                    });
                    if (handle.valid()) {
                        successCount.fetch_add(1, std::memory_order_relaxed);
                        handle.release();
                    }
                }
            });
        }
        
        // Start all threads at the same time
        startFlag.store(true, std::memory_order_release);
        
        // Wait for all threads to complete
        for (auto& t : threads) {
            t.join();
        }
        
        // Wait for any pending operations in the group
        testGroup.wait();
        
        INFO("High contention test completed with success count: " << successCount.load());
        REQUIRE(successCount.load() > 0);
    }
    
    SECTION("Work Execution Under Contention") {
        // Setup outside benchmark scope
        WorkContractGroup benchmarkGroup(groupCapacity);
        std::atomic<int> workCompleted{0};

        
        BENCHMARK("Multi-threaded Work Execution") {

            // Fill the group with simple work up to capacity
            std::vector<WorkContractHandle> handles;
            handles.reserve(groupCapacity);

            for (size_t i = 0; i < groupCapacity; ++i) {
                auto handle = benchmarkGroup.createContract([&workCompleted]() {
                    // Simple work: add two numbers
                    volatile int a = 1, b = 2;
                    volatile int result = a + b;
                    workCompleted.fetch_add(1, std::memory_order_relaxed);
                });
                if (handle.valid()) {
                    handle.schedule();
                    handles.push_back(std::move(handle));
                }
            }

            // Reset work counter for this iteration
            workCompleted.store(0, std::memory_order_relaxed);
            
            // Create worker threads that select and execute work
            std::vector<std::thread> workerThreads;
            workerThreads.reserve(numThreads);
            
            for (int t = 0; t < numThreads; ++t) {
                workerThreads.emplace_back([&benchmarkGroup]() {
                    while (true) {
                        // Select work for execution
                        auto handle = benchmarkGroup.selectForExecution();
                        if (!handle.valid()) {
                            break; // No more work available
                        }
                        
                        // Execute the work
                        benchmarkGroup.executeContract(handle);
                        benchmarkGroup.completeExecution(handle);
                    }
                });
            }
            
            // Wait for all worker threads to complete
            for (auto& t : workerThreads) {
                t.join();
            }
            
            // Wait for any remaining work
            benchmarkGroup.wait();
            
            return workCompleted.load();
        };
    }
}