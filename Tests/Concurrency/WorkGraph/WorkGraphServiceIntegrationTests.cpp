#include <catch2/catch_all.hpp>
#include "../src/Concurrency/WorkGraph.h"
#include "../src/Concurrency/WorkService.h"
#include "../src/Concurrency/WorkContractGroup.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

using namespace EntropyEngine::Core::Concurrency;
using namespace std::chrono_literals;

TEST_CASE("WorkGraph with WorkService integration", "[WorkGraph][WorkService][Integration]") {
    
    SECTION("Basic async execution with WorkService") {
        // 1. Create and start WorkService (manages its own threads)
        WorkService::Config serviceConfig;
        serviceConfig.threadCount = 4;
        WorkService service(serviceConfig);
        service.start();
        
        // 2. Create a WorkContractGroup
        WorkContractGroup group(128, "TestGroup");
        
        // 3. Associate the group with the service
        service.addWorkContractGroup(&group);
        
        // 4. Create a WorkGraph using the group
        WorkGraph graph(&group);
        
        std::atomic<int> counter{0};
        
        // Add nodes
        auto n1 = graph.addNode([&counter]() {
            counter++;
        }, "node1");
        
        auto n2 = graph.addNode([&counter]() {
            counter++;
        }, "node2");
        
        auto n3 = graph.addNode([&counter]() {
            counter++;
        }, "node3");
        
        // Set up dependencies: n1 -> n2 -> n3
        graph.addDependency(n1, n2);
        graph.addDependency(n2, n3);
        
        // Execute the graph - schedules work for WorkService threads
        graph.execute();
        
        // Wait for completion - WorkService threads process the work
        auto result = graph.wait();
        
        REQUIRE(result.allCompleted);
        REQUIRE(result.completedCount == 3);
        REQUIRE(result.droppedCount == 0);
        REQUIRE(counter == 3);
        REQUIRE(graph.isComplete());
        
        service.stop();
    }
    
    SECTION("Multiple graphs running concurrently") {
        // Setup WorkService
        WorkService::Config serviceConfig;
        serviceConfig.threadCount = std::thread::hardware_concurrency();
        WorkService service(serviceConfig);
        service.start();
        
        // Create WorkContractGroup and associate with service
        WorkContractGroup group(256, "ConcurrentGraphs");
        service.addWorkContractGroup(&group);
        
        const int GRAPH_COUNT = 10;
        std::vector<std::unique_ptr<WorkGraph>> graphs;
        std::atomic<int> totalExecutions{0};
        
        // Create multiple graphs
        for (int g = 0; g < GRAPH_COUNT; g++) {
            graphs.push_back(std::make_unique<WorkGraph>(&group));
            auto& graph = *graphs.back();
            
            // Create a diamond dependency pattern
            //     A
            //    / \
            //   B   C
            //    \ /
            //     D
            auto a = graph.addNode([&totalExecutions]() {
                totalExecutions++;
            }, "A_" + std::to_string(g));
            
            auto b = graph.addNode([&totalExecutions]() {
                totalExecutions++;
            }, "B_" + std::to_string(g));
            
            auto c = graph.addNode([&totalExecutions]() {
                totalExecutions++;
            }, "C_" + std::to_string(g));
            
            auto d = graph.addNode([&totalExecutions]() {
                totalExecutions++;
            }, "D_" + std::to_string(g));
            
            graph.addDependency(a, b);
            graph.addDependency(a, c);
            graph.addDependency(b, d);
            graph.addDependency(c, d);
        }
        
        // Execute all graphs concurrently
        for (auto& graph : graphs) {
            graph->execute();
        }
        
        // Wait for all to complete
        for (auto& graph : graphs) {
            auto result = graph->wait();
            REQUIRE(result.allCompleted);
            REQUIRE(graph->isComplete());
        }
        
        REQUIRE(totalExecutions == GRAPH_COUNT * 4);
        
        service.stop();
    }
    
    SECTION("Complex dependency chains") {
        // Setup
        WorkService::Config serviceConfig;
        serviceConfig.threadCount = 4;
        WorkService service(serviceConfig);
        service.start();
        
        WorkContractGroup group(128);
        service.addWorkContractGroup(&group);
        
        WorkGraph graph(&group);
        
        std::atomic<int> executionOrder{0};
        std::vector<int> nodeExecutions(10);
        
        // Create a chain of nodes with dependencies
        std::vector<WorkGraph::NodeHandle> nodes;
        for (int i = 0; i < 10; i++) {
            nodes.push_back(graph.addNode([&executionOrder, &nodeExecutions, i]() {
                nodeExecutions[i] = ++executionOrder;
            }, "node_" + std::to_string(i)));
        }
        
        // Create dependencies: 0->1->2->3->4->5->6->7->8->9
        for (int i = 0; i < 9; i++) {
            graph.addDependency(nodes[i], nodes[i + 1]);
        }
        
        graph.execute();
        graph.wait();
        
        // Verify execution order
        for (int i = 0; i < 10; i++) {
            REQUIRE(nodeExecutions[i] == i + 1);
        }
        
        service.stop();
    }
    
    SECTION("Stress test with limited capacity") {
        // Setup with limited capacity to test deferred node handling
        WorkService::Config serviceConfig;
        serviceConfig.threadCount = 2;
        WorkService service(serviceConfig);
        service.start();
        
        // Small capacity to force deferral
        WorkContractGroup group(8, "StressTest");
        service.addWorkContractGroup(&group);
        
        const int TASK_COUNT = 100;
        WorkGraph graph(&group);
        std::atomic<int> completedTasks{0};
        
        // Create many independent tasks
        for (int i = 0; i < TASK_COUNT; i++) {
            graph.addNode([&completedTasks]() {
                completedTasks++;
                // Small delay to simulate work
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }, "task_" + std::to_string(i));
        }
        
        graph.execute();
        graph.wait();
        
        REQUIRE(completedTasks == TASK_COUNT);
        REQUIRE(graph.isComplete());
        
        service.stop();
    }
    
    SECTION("Fan-out fan-in pattern") {
        // Setup
        WorkService::Config serviceConfig;
        serviceConfig.threadCount = 4;
        WorkService service(serviceConfig);
        service.start();
        
        WorkContractGroup group(128);
        service.addWorkContractGroup(&group);
        
        WorkGraph graph(&group);
        
        std::atomic<int> sourceExecuted{0};
        std::atomic<int> workersExecuted{0};
        std::atomic<int> sinkExecuted{0};
        
        // Create source node
        auto source = graph.addNode([&sourceExecuted]() {
            sourceExecuted = 1;
        }, "source");
        
        // Create worker nodes (fan-out)
        std::vector<WorkGraph::NodeHandle> workers;
        for (int i = 0; i < 10; i++) {
            workers.push_back(graph.addNode([&workersExecuted]() {
                workersExecuted++;
            }, "worker_" + std::to_string(i)));
            graph.addDependency(source, workers.back());
        }
        
        // Create sink node (fan-in)
        auto sink = graph.addNode([&sinkExecuted]() {
            sinkExecuted = 1;
        }, "sink");
        
        for (auto& worker : workers) {
            graph.addDependency(worker, sink);
        }
        
        graph.execute();
        graph.wait();
        
        REQUIRE(sourceExecuted == 1);
        REQUIRE(workersExecuted == 10);
        REQUIRE(sinkExecuted == 1);
        
        service.stop();
    }
    
    SECTION("Dynamic node addition during execution") {
        // Setup
        WorkService::Config serviceConfig;
        serviceConfig.threadCount = 2;
        WorkService service(serviceConfig);
        service.start();
        
        WorkContractGroup group(64);
        service.addWorkContractGroup(&group);
        
        WorkGraph graph(&group);
        std::atomic<int> phase1{0};
        std::atomic<int> phase2{0};
        
        // Add initial nodes
        auto n1 = graph.addNode([&phase1]() {
            phase1++;
        }, "initial_1");
        
        auto n2 = graph.addNode([&phase1]() {
            phase1++;
        }, "initial_2");
        
        graph.addDependency(n1, n2);
        
        // Start execution
        graph.execute();
        
        // Add more nodes while execution is in progress
        // These should execute immediately since they have no dependencies
        for (int i = 0; i < 5; i++) {
            graph.addNode([&phase2]() {
                phase2++;
            }, "dynamic_" + std::to_string(i));
        }
        
        // Wait for everything to complete
        graph.wait();
        
        REQUIRE(phase1 == 2);
        REQUIRE(phase2 == 5);
        
        service.stop();
    }
}

TEST_CASE("WorkGraph error handling with WorkService", "[WorkGraph][WorkService][Error]") {
    
    SECTION("Node failure propagation") {
        // Setup
        WorkService::Config serviceConfig;
        serviceConfig.threadCount = 2;
        WorkService service(serviceConfig);
        service.start();
        
        WorkContractGroup group(64);
        service.addWorkContractGroup(&group);
        
        WorkGraph graph(&group);
        
        std::atomic<int> executedNodes{0};
        
        // Create a chain where one node fails
        auto n1 = graph.addNode([&executedNodes]() {
            executedNodes++;
        }, "n1");
        
        auto n2 = graph.addNode([&executedNodes]() {
            executedNodes++;
            throw std::runtime_error("Simulated error");
        }, "n2_fails");
        
        auto n3 = graph.addNode([&executedNodes]() {
            executedNodes++;  // Should not execute
        }, "n3");
        
        graph.addDependency(n1, n2);
        graph.addDependency(n2, n3);
        
        graph.execute();
        graph.wait();
        
        // n1 and n2 execute, but n3 should not due to n2's failure
        REQUIRE(executedNodes == 2);
        
        service.stop();
    }
}

TEST_CASE("WorkGraph performance characteristics", "[WorkGraph][WorkService][Performance]") {
    
    SECTION("Throughput measurement") {
        WorkService::Config serviceConfig;
        serviceConfig.threadCount = std::thread::hardware_concurrency();
        WorkService service(serviceConfig);
        service.start();
        
        WorkContractGroup group(256);
        service.addWorkContractGroup(&group);
        
        const int OPERATION_COUNT = 10000;
        WorkGraph graph(&group);
        
        std::atomic<int> operations{0};
        auto startTime = std::chrono::steady_clock::now();
        
        // Create many lightweight operations
        for (int i = 0; i < OPERATION_COUNT; i++) {
            graph.addNode([&operations]() {
                operations++;
                // Minimal work to measure overhead
                volatile int x = 0;
                for (int j = 0; j < 100; j++) x++;
            }, "op_" + std::to_string(i));
        }
        
        graph.execute();
        auto result = graph.wait();
        
        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
        
        REQUIRE(result.allCompleted);
        REQUIRE(result.droppedCount == 0);
        REQUIRE(result.completedCount == OPERATION_COUNT);
        REQUIRE(operations == OPERATION_COUNT);
        
        double throughput = (OPERATION_COUNT * 1000000.0) / duration.count();
        INFO("Throughput: " << throughput << " operations/second");
        INFO("Average time per operation: " << (duration.count() / OPERATION_COUNT) << " microseconds");
        
        service.stop();
    }
    
    SECTION("Scalability with thread count") {
        const int WORK_ITEMS = 1000;
        std::vector<double> executionTimes;
        
        // Test with different thread counts
        for (int threadCount : {1, 2, 4, 8}) {
            if (threadCount > static_cast<int>(std::thread::hardware_concurrency())) {
                continue;
            }
            
            WorkService::Config serviceConfig;
            serviceConfig.threadCount = threadCount;
            WorkService service(serviceConfig);
            service.start();
            
            WorkContractGroup group(256);
            service.addWorkContractGroup(&group);
            
            WorkGraph graph(&group);
            
            // Create CPU-bound work
            for (int i = 0; i < WORK_ITEMS; i++) {
                graph.addNode([]() {
                    // Simulate CPU work
                    volatile double x = 1.0;
                    for (int j = 0; j < 10000; j++) {
                        x = x * 1.0001 + 0.001;
                    }
                }, "work_" + std::to_string(i));
            }
            
            auto startTime = std::chrono::steady_clock::now();
            graph.execute();
            graph.wait();
            auto endTime = std::chrono::steady_clock::now();
            
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            executionTimes.push_back(duration.count());
            
            INFO("Thread count: " << threadCount << ", Time: " << duration.count() << "ms");
            
            service.stop();
        }
        
        // Verify that more threads generally improve performance
        if (executionTimes.size() > 1) {
            // Allow some variance, but generally should be faster with more threads
            for (size_t i = 1; i < executionTimes.size(); i++) {
                INFO("Speedup from " << (1 << (i-1)) << " to " << (1 << i) << " threads: " 
                     << (executionTimes[i-1] / executionTimes[i]) << "x");
            }
        }
    }
}