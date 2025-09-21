/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 */

#include <catch2/catch_test_macros.hpp>
#include <Graph/DirectedAcyclicGraph.h>
#include <string>
#include <vector>
#include <set>

using namespace EntropyEngine::Core::Graph;

struct TestNode {
    std::string name;
    int value;
    
    bool operator==(const TestNode& other) const {
        return name == other.name && value == other.value;
    }
};

TEST_CASE("DirectedAcyclicGraph basic operations", "[graph][dag]") {
    SECTION("Empty graph") {
        DirectedAcyclicGraph<TestNode> graph;
        
        // Empty graph has no nodes to iterate
        auto sorted = graph.topologicalSort();
        REQUIRE(sorted.empty());
    }
    
    SECTION("Adding nodes") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node1 = graph.addNode(TestNode{"Node1", 10});
        auto node2 = graph.addNode(TestNode{"Node2", 20});
        
        // Both nodes were added successfully
        REQUIRE(graph.isHandleValid(node1));
        REQUIRE(graph.isHandleValid(node2));
        
        // Verify node data
        REQUIRE(graph.getNodeData(node1)->name == "Node1");
        REQUIRE(graph.getNodeData(node1)->value == 10);
        REQUIRE(graph.getNodeData(node2)->name == "Node2");
        REQUIRE(graph.getNodeData(node2)->value == 20);
    }
    
    SECTION("Removing nodes") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node1 = graph.addNode(TestNode{"Node1", 10});
        auto node2 = graph.addNode(TestNode{"Node2", 20});
        auto node3 = graph.addNode(TestNode{"Node3", 30});
        
        // All three nodes added
        
        graph.removeNode(node2);
        
        // node2 removed, node1 and node3 remain
        REQUIRE(graph.isHandleValid(node1));
        // Note: The handle itself is still "valid" as an object, but the node it points to is gone
        // We can verify this by checking if we can still get data
        REQUIRE(graph.getNodeData(node2) == nullptr);
        REQUIRE(graph.isHandleValid(node3));
    }
    
    SECTION("Node handle validation") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node1 = graph.addNode(TestNode{"Node1", 10});
        REQUIRE(graph.isHandleValid(node1));
        
        graph.removeNode(node1);
        // After removal, getData should return nullptr
        REQUIRE(graph.getNodeData(node1) == nullptr);
    }
}

TEST_CASE("DirectedAcyclicGraph edge operations", "[graph][dag]") {
    SECTION("Adding edges") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node1 = graph.addNode(TestNode{"Node1", 10});
        auto node2 = graph.addNode(TestNode{"Node2", 20});
        auto node3 = graph.addNode(TestNode{"Node3", 30});
        
        // Add valid edges
        graph.addEdge(node1, node2); // 1 -> 2
        graph.addEdge(node2, node3); // 2 -> 3
        
        // Verify connections
        auto children1 = graph.getChildren(node1);
        REQUIRE(children1.size() == 1);
        REQUIRE(children1[0] == node2);
        
        auto children2 = graph.getChildren(node2);
        REQUIRE(children2.size() == 1);
        REQUIRE(children2[0] == node3);
        
        auto parents3 = graph.getParents(node3);
        REQUIRE(parents3.size() == 1);
        REQUIRE(parents3[0] == node2);
    }
    
    SECTION("Cycle detection") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node1 = graph.addNode(TestNode{"Node1", 10});
        auto node2 = graph.addNode(TestNode{"Node2", 20});
        auto node3 = graph.addNode(TestNode{"Node3", 30});
        
        graph.addEdge(node1, node2);
        graph.addEdge(node2, node3);
        
        // Attempt to create a cycle
        REQUIRE_THROWS_AS(graph.addEdge(node3, node1), std::invalid_argument);
        
        // Self-loops should also be rejected
        REQUIRE_THROWS_AS(graph.addEdge(node1, node1), std::invalid_argument);
    }
    
    SECTION("Invalid edge operations") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node1 = graph.addNode(TestNode{"Node1", 10});
        auto node2 = graph.addNode(TestNode{"Node2", 20});
        
        graph.removeNode(node2);
        
        // Invalid handles should throw
        REQUIRE_THROWS_AS(graph.addEdge(node1, node2), std::invalid_argument);
        
        AcyclicNodeHandle<TestNode> invalidHandle;
        REQUIRE_THROWS_AS(graph.addEdge(invalidHandle, node1), std::invalid_argument);
    }
    
    SECTION("Multiple parents and children") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto root = graph.addNode(TestNode{"Root", 0});
        auto child1 = graph.addNode(TestNode{"Child1", 1});
        auto child2 = graph.addNode(TestNode{"Child2", 2});
        auto grandchild = graph.addNode(TestNode{"Grandchild", 3});
        
        // Create diamond shape: root -> child1,child2 -> grandchild
        graph.addEdge(root, child1);
        graph.addEdge(root, child2);
        graph.addEdge(child1, grandchild);
        graph.addEdge(child2, grandchild);
        
        REQUIRE(graph.getChildren(root).size() == 2);
        REQUIRE(graph.getParents(grandchild).size() == 2);
    }
}

TEST_CASE("DirectedAcyclicGraph traversal", "[graph][dag]") {
    SECTION("Topological sort") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node1 = graph.addNode(TestNode{"1", 1});
        auto node2 = graph.addNode(TestNode{"2", 2});
        auto node3 = graph.addNode(TestNode{"3", 3});
        auto node4 = graph.addNode(TestNode{"4", 4});
        
        // Create dependencies: 1 -> 2 -> 4, 1 -> 3 -> 4
        graph.addEdge(node1, node2);
        graph.addEdge(node1, node3);
        graph.addEdge(node2, node4);
        graph.addEdge(node3, node4);
        
        auto sorted = graph.topologicalSort();
        REQUIRE(sorted.size() == 4);
        
        // Find positions in sorted order (sorted contains indices, not handles)
        auto findIndex = [&](const AcyclicNodeHandle<TestNode>& handle) {
            uint32_t targetIdx = handle.handleIndex();
            auto it = std::find(sorted.begin(), sorted.end(), targetIdx);
            return it - sorted.begin();
        };
        
        auto pos1 = findIndex(node1);
        auto pos2 = findIndex(node2);
        auto pos3 = findIndex(node3);
        auto pos4 = findIndex(node4);
        
        // Verify ordering constraints
        REQUIRE(pos1 < pos2);
        REQUIRE(pos1 < pos3);
        REQUIRE(pos2 < pos4);
        REQUIRE(pos3 < pos4);
    }
    
    SECTION("Handle invalidation after removal") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node1 = graph.addNode(TestNode{"1", 1});
        auto node2 = graph.addNode(TestNode{"2", 2});
        graph.addEdge(node1, node2);
        
        // Remove all nodes
        graph.removeNode(node1);
        graph.removeNode(node2);
        
        // After removal, getData should return nullptr
        REQUIRE(graph.getNodeData(node1) == nullptr);
        REQUIRE(graph.getNodeData(node2) == nullptr);
    }
}

TEST_CASE("AcyclicNodeHandle operations", "[graph][dag]") {
    SECTION("Handle comparison") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node1 = graph.addNode(TestNode{"1", 1});
        auto node2 = graph.addNode(TestNode{"2", 2});
        auto node1_copy = node1;
        
        REQUIRE(node1 == node1_copy);
        REQUIRE(node1 != node2);
    }
    
    SECTION("AddChild convenience method") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto parent = graph.addNode(TestNode{"Parent", 0});
        auto child = graph.addNode(TestNode{"Child", 1});
        graph.addEdge(parent, child);
        
        REQUIRE(graph.isHandleValid(child));
        REQUIRE(graph.getChildren(parent).size() == 1);
        REQUIRE(graph.getChildren(parent)[0] == child);
        REQUIRE(graph.getParents(child).size() == 1);
        REQUIRE(graph.getParents(child)[0] == parent);
    }
    
    SECTION("Const correctness") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node = graph.addNode(TestNode{"Test", 42});
        const auto& constNode = node;
        
        // Non-const access
        TestNode* data = graph.getNodeData(node);
        REQUIRE(data != nullptr);
        data->value = 100;
        
        // Const access
        const TestNode* constData = graph.getNodeData(constNode);
        REQUIRE(constData != nullptr);
        REQUIRE(constData->value == 100);
    }
}

TEST_CASE("DirectedAcyclicGraph edge cases", "[graph][dag]") {
    SECTION("Remove node with connections") {
        DirectedAcyclicGraph<TestNode> graph;
        
        auto node1 = graph.addNode(TestNode{"1", 1});
        auto node2 = graph.addNode(TestNode{"2", 2});
        auto node3 = graph.addNode(TestNode{"3", 3});
        
        graph.addEdge(node1, node2);
        graph.addEdge(node2, node3);
        
        graph.removeNode(node2);
        
        // Verify connections are cleaned up
        REQUIRE(graph.getChildren(node1).empty());
        REQUIRE(graph.getParents(node3).empty());
    }
    
    SECTION("Large graph performance") {
        DirectedAcyclicGraph<TestNode> graph;
        std::vector<AcyclicNodeHandle<TestNode>> nodes;
        
        // Create a large graph
        const int nodeCount = 1000;
        for (int i = 0; i < nodeCount; ++i) {
            nodes.push_back(graph.addNode(TestNode{std::to_string(i), i}));
        }
        
        // Create linear chain
        for (int i = 0; i < nodeCount - 1; ++i) {
            graph.addEdge(nodes[i], nodes[i + 1]);
        }
        
        // All nodes added
        
        // Topological sort should still work
        auto sorted = graph.topologicalSort();
        REQUIRE(sorted.size() == nodeCount);
    }
}