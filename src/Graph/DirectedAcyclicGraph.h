/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <stdexcept>
#include <algorithm>
#include <span>
#include <cstdint>

#include "AcyclicNodeHandle.h"
#include "../CoreCommon.h"

namespace EntropyEngine {
namespace Core {
namespace Graph {

    /**
     * @brief Edge storage per node
     */
    struct EdgeList {
        std::vector<uint32_t> outgoing;  ///< Outgoing edges from this node
        std::vector<uint32_t> incoming;  ///< Incoming edges to this node
        
        uint16_t getInDegree() const { return static_cast<uint16_t>(incoming.size()); }
        uint16_t getOutDegree() const { return static_cast<uint16_t>(outgoing.size()); }
    };

    /**
     * @brief Node storage with generation-based handle validation
     *
     * Cache-aligned structure that holds node data and metadata. The generation
     * counter enables detection of stale handles when slots are reused.
     *
     * @tparam T The type of data stored within the node.
     */
    template<class T>
    struct alignas(64) Node {
        T data;                                 ///< Node data payload
        std::atomic<uint32_t> generation{1};    ///< Generation counter for handle validation
        bool occupied{false};                   ///< Whether this slot contains a valid node
        
        /**
         * @brief Default constructor
         */
        Node() = default;

        /**
         * @brief Constructs a Node with initial data
         * @param d The data to move into the node
         * @param occ The initial occupancy status
         */
        Node(T&& d, bool occ) : data(std::move(d)), occupied(occ) {}
        
        /**
         * @brief Move constructor
         * 
         * Resets source node's occupied status after move.
         * @param other The Node to move from
         */
        Node(Node&& other) noexcept 
            : data(std::move(other.data)), 
              generation(other.generation.load()),
              occupied(other.occupied) {
            other.occupied = false;
        }
        
        /**
         * @brief Move assignment operator
         * @param other The Node to move from
         * @return Reference to this Node after assignment
         */
        Node& operator=(Node&& other) noexcept {
            if (this != &other) {
                data = std::move(other.data);
                generation.store(other.generation.load());
                occupied = other.occupied;
                other.occupied = false;
            }
            return *this;
        }
        
        // Non-copyable
        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;
    };

    template<class T>
    /**
     * @brief Cache-friendly directed acyclic graph implementation
     *
     * Manages dependencies between entities while preventing cycles. Uses contiguous
     * storage for cache efficiency and generation-based handles for safe node references.
     * Ideal for task scheduling, build systems, and dependency management.
     *
     * @tparam T The type of data to be stored in each node of the graph.
     */
    class DirectedAcyclicGraph {
        // Hot data - frequently accessed together
        std::vector<Node<T>> _nodes;           ///< Contiguous storage for all graph nodes. Designed for cache efficiency.
        std::vector<EdgeList> _edges;          ///< Simple edge storage per node for robustness.
        
        // Cold data - rarely accessed
        std::queue<uint32_t> _freeList;        ///< A queue of indices for recently freed node slots, enabling reuse and reducing memory fragmentation.
        
        // Make AcyclicNodeHandle a friend for all T types
        template<typename U>
        friend class AcyclicNodeHandle;
        
    public:
        /**
         * @brief Constructs a new DirectedAcyclicGraph instance
         *
         * Pre-allocates storage for 64 nodes to reduce initial reallocations.
         *
         * @code
         * // Create a graph to store integer data
         * DirectedAcyclicGraph<int> myGraph;
         *
         * // Add some nodes
         * auto node1 = myGraph.addNode(10);
         * auto node2 = myGraph.addNode(20);
         * @endcode
         */
        DirectedAcyclicGraph() {
            _nodes.reserve(64);
            _edges.reserve(64);
        }

        /**
         * @brief Adds a new node to the graph
         *
         * Reuses freed slots when available, otherwise extends storage.
         *
         * @param data The data to store in the node
         * @return Handle to the newly added node
         *
         * @code
         * DirectedAcyclicGraph<std::string> stringGraph;
         * auto taskA = stringGraph.addNode("Download Data");
         * auto taskB = stringGraph.addNode("Process Data");
         * auto taskC = stringGraph.addNode("Upload Results");
         *
         * // taskA, taskB, taskC are now valid handles to nodes in the graph.
         * @endcode
         */
        AcyclicNodeHandle<T> addNode(T data) {
            uint32_t index;
            
            // Prefer recently freed slots (likely still in cache)
            if (!_freeList.empty()) {
                index = _freeList.front();
                _freeList.pop();
                _nodes[index] = std::move(Node<T>{std::move(data), true});
                // Generation already incremented during removal
            } else {
                index = static_cast<uint32_t>(_nodes.size());
                _nodes.emplace_back(Node<T>{std::move(data), true});
                _edges.emplace_back();
            }
            
            return AcyclicNodeHandle<T>(this, index, _nodes[index].generation.load());
        }

        /**
         * @brief Removes a node from the graph
         *
         * Invalidates the handle and removes all connected edges.
         *
         * @param node Handle of the node to remove
         * @return true if successfully removed, false if handle was invalid
         *
         * @code
         * DirectedAcyclicGraph<int> graph;
         * auto node1 = graph.addNode(1);
         * auto node2 = graph.addNode(2);
         * graph.addEdge(node1, node2);
         *
         * bool removed = graph.removeNode(node1);
         * // removed will be true
         * bool isValid = graph.isHandleValid(node1);
         * // isValid will be false
         * @endcode
         */
        bool removeNode(AcyclicNodeHandle<T> node) {
            if (!isHandleValid(node)) {
                return false;
            }
            
            uint32_t index = node.handleIndex();
            
            // Remove all edges from and to this node
            removeAllEdges(index);
            
            // Mark as unoccupied and increment generation
            _nodes[index].occupied = false;
            _nodes[index].generation.fetch_add(1, std::memory_order_relaxed);
            
            // Add to free list for reuse
            _freeList.push(index);
            
            return true;
        }

        /**
         * @brief Removes a directed edge between two nodes
         *
         * @param from The source node handle
         * @param to The target node handle
         * @return true if edge was removed, false if it didn't exist
         */
        bool removeEdge(AcyclicNodeHandle<T> from, AcyclicNodeHandle<T> to) {
            if (!isHandleValid(from) || !isHandleValid(to)) {
                return false;
            }
            
            uint32_t fromIdx = from.handleIndex();
            uint32_t toIdx = to.handleIndex();
            
            // Remove from outgoing edges of source
            auto& outgoing = _edges[fromIdx].outgoing;
            auto outIt = std::find(outgoing.begin(), outgoing.end(), toIdx);
            if (outIt == outgoing.end()) {
                return false; // Edge doesn't exist
            }
            outgoing.erase(outIt);
            
            // Remove from incoming edges of target
            auto& incoming = _edges[toIdx].incoming;
            auto inIt = std::find(incoming.begin(), incoming.end(), fromIdx);
            if (inIt != incoming.end()) {
                incoming.erase(inIt);
            }
            
            return true;
        }

        /**
         * @brief Adds a directed edge from one node to another
         *
         * Establishes a dependency where `from` must precede `to`. Prevents cycles.
         *
         * @param from Source node handle
         * @param to Destination node handle
         * @throws std::invalid_argument If handles invalid, self-loop, or would create cycle
         *
         * @code
         * DirectedAcyclicGraph<std::string> buildGraph;
         * auto compile = buildGraph.addNode("Compile Source");
         * auto link = buildGraph.addNode("Link Executable");
         * auto deploy = buildGraph.addNode("Deploy Application");
         *
         * try {
         *     buildGraph.addEdge(compile, link); // Link depends on compile
         *     buildGraph.addEdge(link, deploy);   // Deploy depends on link
         *     // buildGraph.addEdge(deploy, compile); // This would throw std::invalid_argument (cycle detected)
         * } catch (const std::invalid_argument& e) {
         *     std::cerr << "Error adding edge: " << e.what() << std::endl;
         * }
         * @endcode
         */
        void addEdge(AcyclicNodeHandle<T> from, AcyclicNodeHandle<T> to) {
            if (!isHandleValid(from) || !isHandleValid(to)) {
                throw std::invalid_argument("Invalid handle provided to addEdge");
            }
            
            uint32_t fromIdx = from.handleIndex();
            uint32_t toIdx = to.handleIndex();
            
            // Check for self-loops
            if (fromIdx == toIdx) {
                throw std::invalid_argument("Self-loops are not allowed in acyclic graph");
            }
            
            // Check if edge already exists
            if (hasEdge(fromIdx, toIdx)) {
                return; // Edge already exists
            }
            
            // Check for cycles using DFS
            if (wouldCreateCycle(fromIdx, toIdx)) {
                throw std::invalid_argument("Adding edge would create a cycle");
            }
            
            // Add the forward edge
            _edges[fromIdx].outgoing.push_back(toIdx);
            
            // Add the reverse edge
            _edges[toIdx].incoming.push_back(fromIdx);
        }

        /**
         * @brief Gets mutable pointer to node data
         *
         * Returns nullptr if the handle is invalid.
         *
         * @param node Handle to the node
         * @return Pointer to the node's data, or nullptr if invalid
         *
         * @code
         * DirectedAcyclicGraph<int> graph;
         * auto nodeHandle = graph.addNode(100);
         * int* data = graph.getNodeData(nodeHandle);
         * if (data) {
         *     *data = 200; // Modify the node's data
         * }
         * @endcode
         */
        T* getNodeData(AcyclicNodeHandle<T> node) {
            if (!isHandleValid(node)) {
                return nullptr;
            }
            return &_nodes[node.handleIndex()].data;
        }

        /**
         * @brief Gets const pointer to node data
         *
         * Provides read-only access with handle validation.
         *
         * @param node Handle to the node
         * @return Const pointer to the node's data, or nullptr if invalid
         *
         * @code
         * DirectedAcyclicGraph<std::string> graph;
         * auto nodeHandle = graph.addNode("Hello World");
         * const std::string* data = graph.getNodeData(nodeHandle);
         * if (data) {
         *     std::cout << *data << std::endl; // Read the node's data
         * }
         * @endcode
         */
        const T* getNodeData(AcyclicNodeHandle<T> node) const {
            if (!isHandleValid(node)) {
                return nullptr;
            }
            return &_nodes[node.handleIndex()].data;
        }
        
        /**
         * @brief Validates a node handle
         *
         * Checks if the handle refers to an existing node by verifying index bounds,
         * occupancy status, and generation count.
         *
         * @param handle The handle to validate
         * @return true if handle is valid and points to an active node
         *
         * @code
         * DirectedAcyclicGraph<int> graph;
         * auto node1 = graph.addNode(10);
         * auto node2 = graph.addNode(20);
         *
         * bool valid1 = graph.isHandleValid(node1); // true
         * graph.removeNode(node1);
         * bool valid1_after_remove = graph.isHandleValid(node1); // false
         * @endcode
         */
        bool isHandleValid(const AcyclicNodeHandle<T>& handle) const {
            auto* owner = handle.template handleOwnerAs<DirectedAcyclicGraph<T>>();
            if (owner != this) return false;
            
            uint32_t index = handle.handleIndex();
            if (index >= _nodes.size()) return false;
            
            return _nodes[index].occupied && 
                   _nodes[index].generation.load() == handle.handleGeneration();
        }
        
        /**
         * @brief Checks if a directed edge exists between two nodes
         *
         * @param from Index of the source node
         * @param to Index of the destination node
         * @return true if edge exists from `from` to `to`
         *
         * @code
         * DirectedAcyclicGraph<int> graph;
         * auto nodeA = graph.addNode(1);
         * auto nodeB = graph.addNode(2);
         * graph.addEdge(nodeA, nodeB);
         *
         * bool has = graph.hasEdge(nodeA.getIndex(), nodeB.getIndex()); // true
         * bool has_reverse = graph.hasEdge(nodeB.getIndex(), nodeA.getIndex()); // false
         * @endcode
         */
        bool hasEdge(uint32_t from, uint32_t to) const {
            const auto& outgoing = _edges[from].outgoing;
            return std::find(outgoing.begin(), outgoing.end(), to) != outgoing.end();
        }
        
        /**
         * @brief Checks if adding an edge would create a cycle
         *
         * Uses DFS to check if `from` is reachable from `to`.
         *
         * @param from Index of potential source node
         * @param to Index of potential destination node
         * @return true if adding the edge would create a cycle
         *
         * @code
         * DirectedAcyclicGraph<int> graph;
         * auto n1 = graph.addNode(1);
         * auto n2 = graph.addNode(2);
         * auto n3 = graph.addNode(3);
         * graph.addEdge(n1, n2);
         * graph.addEdge(n2, n3);
         *
         * // This would create a cycle: n3 -> n1 -> n2 -> n3
         * bool createsCycle = graph.wouldCreateCycle(n3.getIndex(), n1.getIndex()); // true
         * @endcode
         */
        bool wouldCreateCycle(uint32_t from, uint32_t to) const {
            // DFS from 'to' to see if we can reach 'from'
            std::vector<bool> visited(_nodes.size(), false);
            std::vector<uint32_t> stack;
            stack.push_back(to);
            
            while (!stack.empty()) {
                uint32_t current = stack.back();
                stack.pop_back();
                
                if (current == from) {
                    return true; // Found cycle
                }
                
                if (visited[current]) {
                    continue;
                }
                visited[current] = true;
                
                // Add all neighbors
                const auto& outgoing = _edges[current].outgoing;
                for (uint32_t neighbor : outgoing) {
                    if (!visited[neighbor]) {
                        stack.push_back(neighbor);
                    }
                }
            }
            
            return false;
        }
        
        /**
         * @brief Removes all edges connected to a node
         *
         * Disconnects both incoming and outgoing edges. O(degree) complexity.
         *
         * @param nodeIndex Index of the node to disconnect
         *
         * @code
         * DirectedAcyclicGraph<int> graph;
         * auto n1 = graph.addNode(1);
         * auto n2 = graph.addNode(2);
         * auto n3 = graph.addNode(3);
         * graph.addEdge(n1, n2);
         * graph.addEdge(n3, n2);
         *
         * graph.removeAllEdges(n2.getIndex());
         * // Now, n1 -> n2 and n3 -> n2 edges are removed.
         * @endcode
         */
        void removeAllEdges(uint32_t nodeIndex) {
            // Remove outgoing edges
            auto& outgoing = _edges[nodeIndex].outgoing;
            
            // For each outgoing edge, remove from target's incoming edges
            for (uint32_t target : outgoing) {
                auto& targetIncoming = _edges[target].incoming;
                targetIncoming.erase(std::remove(targetIncoming.begin(), targetIncoming.end(), nodeIndex), targetIncoming.end());
            }
            
            outgoing.clear();
            
            // Remove incoming edges
            auto& incoming = _edges[nodeIndex].incoming;
            
            // For each incoming edge, remove from source's outgoing edges
            for (uint32_t source : incoming) {
                auto& sourceOutgoing = _edges[source].outgoing;
                sourceOutgoing.erase(std::remove(sourceOutgoing.begin(), sourceOutgoing.end(), nodeIndex), sourceOutgoing.end());
            }
            
            incoming.clear();
        }
        
        
        /**
         * @brief Gets outgoing edges for a node
         *
         * @param nodeIndex Index of the node
         * @return Span of indices this node points to
         *
         * @code
         * DirectedAcyclicGraph<int> graph;
         * auto n1 = graph.addNode(1);
         * auto n2 = graph.addNode(2);
         * auto n3 = graph.addNode(3);
         * graph.addEdge(n1, n2);
         * graph.addEdge(n1, n3);
         *
         * for (uint32_t neighborIndex : graph.getOutgoingEdges(n1.getIndex())) {
         *     // Process neighborIndex (e.g., get its data)
         *     std::cout << "Node 1 has outgoing edge to: " << neighborIndex << std::endl;
         * }
         * @endcode
         */
        std::span<const uint32_t> getOutgoingEdges(uint32_t nodeIndex) const {
            const auto& outgoing = _edges[nodeIndex].outgoing;
            return std::span{outgoing.data(), outgoing.size()};
        }
        
        /**
         * @brief Gets incoming edges for a node
         *
         * @param nodeIndex Index of the node
         * @return Span of indices that point to this node
         *
         * @code
         * DirectedAcyclicGraph<int> graph;
         * auto n1 = graph.addNode(1);
         * auto n2 = graph.addNode(2);
         * auto n3 = graph.addNode(3);
         * graph.addEdge(n1, n3);
         * graph.addEdge(n2, n3);
         *
         * for (uint32_t predecessorIndex : graph.getIncomingEdges(n3.getIndex())) {
         *     // Process predecessorIndex
         *     std::cout << "Node 3 has incoming edge from: " << predecessorIndex << std::endl;
         * }
         * @endcode
         */
        std::span<const uint32_t> getIncomingEdges(uint32_t nodeIndex) const {
            const auto& incoming = _edges[nodeIndex].incoming;
            return std::span{incoming.data(), incoming.size()};
        }
        
        /**
         * @brief Gets the children of a node as handles
         *
         * @param node Parent node handle
         * @return Vector of child node handles
         *
         * @code
         * auto parent = graph.addNode("Parent");
         * auto child1 = graph.addNode("Child 1");
         * auto child2 = graph.addNode("Child 2");
         * graph.addEdge(parent, child1);
         * graph.addEdge(parent, child2);
         *
         * auto children = graph.getChildren(parent);
         * for (auto child : children) {
         *     std::cout << "Child: " << child.getData()->name << std::endl;
         * }
         * @endcode
         */
        std::vector<AcyclicNodeHandle<T>> getChildren(const AcyclicNodeHandle<T>& node) const {
            // Const version delegates to non-const version
            return const_cast<DirectedAcyclicGraph<T>*>(this)->getChildren(node);
        }

        std::vector<AcyclicNodeHandle<T>> getChildren(const AcyclicNodeHandle<T>& node) {
            std::vector<AcyclicNodeHandle<T>> children;
            if (!isHandleValid(node)) return children;
            
            auto childIndices = getOutgoingEdges(node.handleIndex());
            children.reserve(childIndices.size());
            
            for (uint32_t childIndex : childIndices) {
                if (childIndex < _nodes.size() && _nodes[childIndex].occupied) {
                    children.emplace_back(this, childIndex, _nodes[childIndex].generation.load());
                }
            }
            
            return children;
        }

        /**
         * @brief Gets the parents of a node as handles
         *
         * @param node The child node handle
         * @return Vector of parent node handles
         *
         * @code
         * auto parent1 = graph.addNode("Parent 1");
         * auto parent2 = graph.addNode("Parent 2");
         * auto child = graph.addNode("Child");
         * graph.addEdge(parent1, child);
         * graph.addEdge(parent2, child);
         *
         * auto parents = graph.getParents(child);
         * for (auto parent : parents) {
         *     std::cout << "Parent: " << parent.getData()->name << std::endl;
         * }
         * @endcode
         */
        std::vector<AcyclicNodeHandle<T>> getParents(const AcyclicNodeHandle<T>& node) const {
            // Const version delegates to non-const version
            return const_cast<DirectedAcyclicGraph<T>*>(this)->getParents(node);
        }

        std::vector<AcyclicNodeHandle<T>> getParents(const AcyclicNodeHandle<T>& node) {
            std::vector<AcyclicNodeHandle<T>> parents;
            if (!isHandleValid(node)) return parents;
            
            auto parentIndices = getIncomingEdges(node.handleIndex());
            parents.reserve(parentIndices.size());
            
            for (uint32_t parentIndex : parentIndices) {
                if (parentIndex < _nodes.size() && _nodes[parentIndex].occupied) {
                    parents.emplace_back(this, parentIndex, _nodes[parentIndex].generation.load());
                }
            }
            
            return parents;
        }

        /**
         * @brief Performs topological sort on the graph
         *
         * Returns nodes in dependency order using Kahn's algorithm. For every edge
         * u -> v, node u appears before v in the result.
         *
         * @return Vector of node indices in topological order
         *
         * @code
         * DirectedAcyclicGraph<std::string> taskGraph;
         * auto compile = taskGraph.addNode("Compile Source");
         * auto link = taskGraph.addNode("Link Executable");
         * auto test = taskGraph.addNode("Run Tests");
         * auto deploy = taskGraph.addNode("Deploy Application");
         *
         * taskGraph.addEdge(compile, link);
         * taskGraph.addEdge(link, test);
         * taskGraph.addEdge(test, deploy);
         *
         * std::vector<uint32_t> order = taskGraph.topologicalSort();
         * for (uint32_t nodeIndex : order) {
         *     // In a real scenario, you'd get the node data and execute the task
         *     std::cout << "Executing: " << *taskGraph.getNodeData(AcyclicNodeHandle<std::string>(nullptr, nodeIndex, 0)) << std::endl;
         * }
         * // Expected output order: Compile Source, Link Executable, Run Tests, Deploy Application
         * @endcode
         */
        std::vector<uint32_t> topologicalSort() const {
            std::vector<uint32_t> result;
            result.reserve(_nodes.size());
            
            // Use compact in-degree array
            std::vector<uint16_t> inDegrees(_nodes.size(), 0);
            
            // Calculate in-degrees
            for (uint32_t i = 0; i < _nodes.size(); ++i) {
                if (!_nodes[i].occupied) continue;
                inDegrees[i] = _edges[i].getInDegree();
            }
            
            // Find nodes with zero in-degree
            std::vector<uint32_t> zeroInDegree;
            zeroInDegree.reserve(_nodes.size() / 4);
            
            for (uint32_t i = 0; i < _nodes.size(); ++i) {
                if (inDegrees[i] == 0 && _nodes[i].occupied) {
                    zeroInDegree.push_back(i);
                }
            }
            
            // Process with good locality
            size_t processIdx = 0;
            while (processIdx < zeroInDegree.size()) {
                uint32_t current = zeroInDegree[processIdx++];
                result.push_back(current);
                
                const auto& outgoing = _edges[current].outgoing;
                
                // Sequential edge access
                for (uint32_t target : outgoing) {
                    if (--inDegrees[target] == 0) {
                        zeroInDegree.push_back(target);
                    }
                }
            }
            
            return result;
        }

    };



} // namespace Graph
} // namespace Core
} // namespace EntropyEngine

