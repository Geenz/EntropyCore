#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "Concurrency/WorkGraph.h"
#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkService.h"
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <sstream>

using namespace EntropyEngine::Core::Concurrency;
using namespace std::chrono_literals;

SCENARIO("WorkGraph basic functionality", "[workgraph][experimental][basic]") {
    GIVEN("A work graph with a contract group") {
        WorkContractGroup contractGroup(256);
        WorkGraph graph(&contractGroup);
        
        WHEN("A single node is added and executed") {
            std::atomic<bool> executed{false};
            
            auto node = graph.addNode([&executed]() {
                executed = true;
            }, "single_node");
            
            graph.execute();
            
            // Execute the contracts
            contractGroup.executeAllBackgroundWork();
            
            THEN("The node should execute") {
                REQUIRE(executed == true);
                REQUIRE(graph.isComplete());
            }
        }
        
        WHEN("Multiple independent nodes are added") {
            std::atomic<int> executionCount{0};
            
            auto node1 = graph.addNode([&executionCount]() {
                executionCount.fetch_add(1);
            }, "node1");
            
            auto node2 = graph.addNode([&executionCount]() {
                executionCount.fetch_add(1);
            }, "node2");
            
            auto node3 = graph.addNode([&executionCount]() {
                executionCount.fetch_add(1);
            }, "node3");
            
            graph.execute();
            contractGroup.executeAllBackgroundWork();
            
            THEN("All nodes should execute") {
                REQUIRE(executionCount == 3);
                REQUIRE(graph.isComplete());
            }
        }
    }
}

SCENARIO("WorkGraph dependency ordering", "[workgraph][experimental][dependencies]") {
    GIVEN("A work graph with linear dependencies") {
        WorkContractGroup contractGroup(256);
        WorkGraph graph(&contractGroup);
        
        std::vector<int> executionOrder;
        std::mutex orderMutex;
        
        auto recordExecution = [&executionOrder, &orderMutex](int nodeId) {
            std::lock_guard<std::mutex> lock(orderMutex);
            executionOrder.push_back(nodeId);
        };
        
        WHEN("A->B->C dependency chain is created") {
            auto nodeA = graph.addNode([&]() { recordExecution(1); }, "A");
            auto nodeB = graph.addNode([&]() { recordExecution(2); }, "B");
            auto nodeC = graph.addNode([&]() { recordExecution(3); }, "C");
            
            graph.addDependency(nodeA, nodeB); // B depends on A
            graph.addDependency(nodeB, nodeC); // C depends on B
            
            graph.execute();
            
            // Execute all work
            while (!graph.isComplete()) {
                contractGroup.executeAllBackgroundWork();
                std::this_thread::sleep_for(1ms);
            }
            
            THEN("Execution order should be A, B, C") {
                REQUIRE(executionOrder.size() == 3);
                REQUIRE(executionOrder[0] == 1);
                REQUIRE(executionOrder[1] == 2);
                REQUIRE(executionOrder[2] == 3);
            }
        }
        
        WHEN("Diamond dependency pattern is created") {
            //     A
            //    / \
            //   B   C
            //    \ /
            //     D
            
            auto nodeA = graph.addNode([&]() { recordExecution(1); }, "A");
            auto nodeB = graph.addNode([&]() { recordExecution(2); }, "B");
            auto nodeC = graph.addNode([&]() { recordExecution(3); }, "C");
            auto nodeD = graph.addNode([&]() { recordExecution(4); }, "D");
            
            graph.addDependency(nodeA, nodeB); // B depends on A
            graph.addDependency(nodeA, nodeC); // C depends on A
            graph.addDependency(nodeB, nodeD); // D depends on B
            graph.addDependency(nodeC, nodeD); // D depends on C
            
            graph.execute();
            
            // Execute all work
            while (!graph.isComplete()) {
                contractGroup.executeAllBackgroundWork();
                std::this_thread::sleep_for(1ms);
            }
            
            THEN("A executes first, B and C can be in any order, D executes last") {
                REQUIRE(executionOrder.size() == 4);
                REQUIRE(executionOrder[0] == 1); // A first
                REQUIRE(executionOrder[3] == 4); // D last
                // B and C can be in either order
                REQUIRE(((executionOrder[1] == 2 && executionOrder[2] == 3) ||
                        (executionOrder[1] == 3 && executionOrder[2] == 2)));
            }
        }
    }
}

SCENARIO("WorkGraph parallel execution", "[workgraph][experimental][parallel]") {
    GIVEN("A work graph with parallel branches") {
        WorkService::Config config;
        config.threadCount = 4;
        WorkService service(config);
        
        WorkContractGroup contractGroup(256);
        service.addWorkContractGroup(&contractGroup);
        service.start();
        
        WHEN("Multiple independent branches are created") {
            WorkGraph graph(&contractGroup);
            std::atomic<int> concurrentExecutions{0};
            std::atomic<int> maxConcurrent{0};
            
            auto work = [&concurrentExecutions, &maxConcurrent]() {
                int current = concurrentExecutions.fetch_add(1) + 1;
                
                // Update max concurrent
                int expected = maxConcurrent.load();
                while (expected < current && 
                       !maxConcurrent.compare_exchange_weak(expected, current)) {
                }
                
                // Simulate work
                std::this_thread::sleep_for(10ms);
                
                concurrentExecutions.fetch_sub(1);
            };
            
            // Create root
            auto root = graph.addNode([]() {}, "root");
            
            // Create multiple branches
            for (int i = 0; i < 8; ++i) {
                auto branch = graph.addNode(work, "branch_" + std::to_string(i));
                graph.addDependency(root, branch);
            }
            
            graph.execute();
            graph.wait();
            
            THEN("Multiple nodes should execute concurrently") {
                REQUIRE(maxConcurrent.load() > 1);
                REQUIRE(graph.isComplete());
            }
        }
        
        service.stop();
    }
}

SCENARIO("WorkGraph dynamic construction", "[workgraph][experimental][dynamic]") {
    GIVEN("A work graph that builds dynamically") {
        WorkService::Config config;
        config.threadCount = 2;
        WorkService service(config);
        
        WorkContractGroup contractGroup(256);
        service.addWorkContractGroup(&contractGroup);
        service.start();
        
        WHEN("Nodes create child nodes during execution") {
            WorkGraph graph(&contractGroup);
            std::atomic<int> totalExecutions{0};
            
            // Function to create child nodes
            std::function<void(int)> createChildren;
            createChildren = [&](int depth) {
                totalExecutions.fetch_add(1);
                
                if (depth > 0) {
                    // Create two children
                    auto child1 = graph.addNode([&, depth]() {
                        createChildren(depth - 1);
                    }, "child_depth_" + std::to_string(depth));
                    
                    auto child2 = graph.addNode([&, depth]() {
                        createChildren(depth - 1);
                    }, "child_depth_" + std::to_string(depth));
                    
                    // Note: We can't add dependencies to already-executing nodes
                    // So these will execute independently
                }
            };
            
            auto root = graph.addNode([&]() {
                createChildren(2);
            }, "root");
            
            graph.execute();
            
            // Wait for all dynamically created work
            std::this_thread::sleep_for(100ms);
            
            THEN("All dynamically created nodes should execute") {
                // 1 root + 2 depth-2 + 4 depth-1 = 7 total
                REQUIRE(totalExecutions.load() == 7);
            }
        }
        
        service.stop();
    }
}

SCENARIO("WorkGraph continuation patterns", "[workgraph][experimental][continuation]") {
    GIVEN("A work graph with continuation support") {
        WorkContractGroup contractGroup(256);
        WorkGraph graph(&contractGroup);
        
        WHEN("A continuation is created for multiple parents") {
            std::atomic<int> parent1Done{0};
            std::atomic<int> parent2Done{0};
            std::atomic<int> parent3Done{0};
            std::atomic<bool> continuationDone{false};
            
            auto parent1 = graph.addNode([&]() {
                std::this_thread::sleep_for(10ms);
                parent1Done = 1;
            }, "parent1");
            
            auto parent2 = graph.addNode([&]() {
                std::this_thread::sleep_for(20ms);
                parent2Done = 1;
            }, "parent2");
            
            auto parent3 = graph.addNode([&]() {
                std::this_thread::sleep_for(5ms);
                parent3Done = 1;
            }, "parent3");
            
            // Create continuation that runs after all parents
            auto continuation = graph.addContinuation(
                {parent1, parent2, parent3},
                [&]() {
                    // All parents should be done
                    REQUIRE(parent1Done == 1);
                    REQUIRE(parent2Done == 1);
                    REQUIRE(parent3Done == 1);
                    continuationDone = true;
                },
                "continuation"
            );
            
            graph.execute();
            
            // Execute work
            while (!graph.isComplete()) {
                contractGroup.executeAllBackgroundWork();
                std::this_thread::sleep_for(1ms);
            }
            
            THEN("Continuation runs after all parents complete") {
                REQUIRE(continuationDone == true);
            }
        }
    }
}

SCENARIO("WorkGraph error handling", "[workgraph][experimental][errors]") {
    GIVEN("A work graph with potential errors") {
        WorkContractGroup contractGroup(256);
        WorkGraph graph(&contractGroup);
        
        WHEN("A cycle is attempted") {
            auto node1 = graph.addNode([]() {}, "node1");
            auto node2 = graph.addNode([]() {}, "node2");
            auto node3 = graph.addNode([]() {}, "node3");
            
            graph.addDependency(node1, node2);
            graph.addDependency(node2, node3);
            
            THEN("Adding a cycle should throw") {
                REQUIRE_THROWS_AS(
                    graph.addDependency(node3, node1),
                    std::invalid_argument
                );
            }
        }
        
        WHEN("Invalid node handles are used") {
            WorkGraph otherGraph(&contractGroup);
            auto node1 = graph.addNode([]() {}, "node1");
            auto node2 = otherGraph.addNode([]() {}, "node2");
            
            THEN("Cross-graph dependency should throw") {
                REQUIRE_THROWS_AS(
                    graph.addDependency(node1, node2),
                    std::invalid_argument
                );
            }
        }
    }
}

SCENARIO("WorkGraph completion callback", "[workgraph][experimental][callback]") {
    GIVEN("A work graph with completion tracking") {
        WorkContractGroup contractGroup(256);
        WorkGraph graph(&contractGroup);
        
        std::vector<std::string> completionOrder;
        std::mutex orderMutex;
        
        graph.setNodeCompleteCallback([&](WorkGraph::NodeHandle node) {
            if (auto* nodeData = graph.getNodeData(node)) {
                std::lock_guard<std::mutex> lock(orderMutex);
                completionOrder.push_back(nodeData->name);
            }
        });
        
        WHEN("Nodes complete in dependency order") {
            auto nodeA = graph.addNode([]() {}, "A");
            auto nodeB = graph.addNode([]() {}, "B");
            auto nodeC = graph.addNode([]() {}, "C");
            
            graph.addDependency(nodeA, nodeB);
            graph.addDependency(nodeB, nodeC);
            
            graph.execute();
            
            while (!graph.isComplete()) {
                contractGroup.executeAllBackgroundWork();
                std::this_thread::sleep_for(1ms);
            }
            
            THEN("Callbacks are called in completion order") {
                REQUIRE(completionOrder.size() == 3);
                REQUIRE(completionOrder[0] == "A");
                REQUIRE(completionOrder[1] == "B");
                REQUIRE(completionOrder[2] == "C");
            }
        }
    }
}

SCENARIO("WorkGraph stress test", "[workgraph][experimental][stress][!mayfail]") {
    GIVEN("A large work graph") {
        WorkService::Config config;
        config.threadCount = std::thread::hardware_concurrency();
        WorkService service(config);
        
        WorkContractGroup contractGroup(1024);
        service.addWorkContractGroup(&contractGroup);
        service.start();
        
        WHEN("Many nodes with complex dependencies are created") {
            WorkGraph graph(&contractGroup);
            const int layers = 3;
            const int nodesPerLayer = 10;
            std::atomic<int> executionCount{0};
            
            std::vector<std::vector<WorkGraph::NodeHandle>> layerNodes(layers);
            
            // Create layers
            for (int layer = 0; layer < layers; ++layer) {
                for (int node = 0; node < nodesPerLayer; ++node) {
                    std::string nodeName = "layer_" + std::to_string(layer) + "_node_" + std::to_string(node);
                    auto handle = graph.addNode([&executionCount]() {
                        // INFO("Executing node: " << nodeName);  // Can't capture nodeName safely
                        executionCount.fetch_add(1);
                        std::this_thread::sleep_for(1ms);
                    }, nodeName);
                    
                    layerNodes[layer].push_back(handle);
                }
            }
            
            // Create dependencies between layers
            // Simple approach: each node in layer N depends on all nodes in layer N-1
            for (int layer = 1; layer < layers; ++layer) {
                for (int nodeIdx = 0; nodeIdx < nodesPerLayer; ++nodeIdx) {
                    auto& child = layerNodes[layer][nodeIdx];
                    
                    // Add dependency on all nodes from previous layer
                    for (int parentIdx = 0; parentIdx < nodesPerLayer; ++parentIdx) {
                        graph.addDependency(layerNodes[layer-1][parentIdx], child);
                    }
                }
            }
            
            
            auto startTime = std::chrono::steady_clock::now();
            
            graph.execute();
            
            // Use wait() as requested by the user
            graph.wait();
            
            auto endTime = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            
            THEN("All nodes should execute efficiently") {
                INFO("Pending count: " << graph.getPendingCount());
                INFO("Execution count: " << executionCount.load());
                INFO("Expected: " << layers * nodesPerLayer);
                
                REQUIRE(executionCount == layers * nodesPerLayer);
                REQUIRE(graph.isComplete());
                
                INFO("Execution time: " << duration.count() << "ms");
                INFO("Nodes executed: " << executionCount.load());
            }
        }
        
        service.stop();
    }
}