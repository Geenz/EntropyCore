/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#pragma once

#include <atomic>

#include "../Core/EventBus.h"
#include "WorkGraphTypes.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

/**
 * @brief Centralized state management for WorkGraph nodes
 *
 * This component is responsible for:
 * - Validating state transitions
 * - Publishing state change events
 * - Tracking state statistics
 * - Providing thread-safe state queries
 *
 * Design goals:
 * - Minimal memory footprint (suitable for thousands of instances)
 * - Lock-free reads where possible
 * - Optional event publishing (only if EventBus provided)
 */
class NodeStateManager
{
public:
    /**
     * @brief Construct state manager
     *
     * @param graph The owning WorkGraph (for event context)
     * @param eventBus Optional event bus for publishing state changes
     */
    explicit NodeStateManager(const WorkGraph* graph, Core::EventBus* eventBus = nullptr)
        : _graph(graph), _eventBus(eventBus) {}

    /**
     * @brief Attempt to transition a node to a new state
     *
     * This is the primary method for all state changes. It validates the transition,
     * updates the state atomically, and publishes events if configured.
     *
     * @param node The node to transition
     * @param from Expected current state (for CAS operation)
     * @param to Target state
     * @return true if transition succeeded, false if current state didn't match 'from'
     */
    bool transitionState(const NodeHandle& node, NodeState from, NodeState to);

    /**
     * @brief Force a state transition without validation
     *
     * Use sparingly - only for error recovery or initialization
     *
     * @param node The node to transition
     * @param to Target state
     */
    void forceState(const NodeHandle& node, NodeState to);

    /**
     * @brief Get current state of a node
     *
     * @param node The node to query
     * @return Current state, or Pending if node is invalid
     */
    NodeState getState(const NodeHandle& node) const;

    /**
     * @brief Check if a state transition is valid
     *
     * @param from Source state
     * @param to Target state
     * @return true if transition is allowed
     */
    static bool canTransition(NodeState from, NodeState to) {
        return isValidTransition(from, to);
    }

    /**
     * @brief Get human-readable name for a state
     * @param state The state to convert
     * @return String representation of the state
     */
    static const char* getStateName(NodeState state) {
        return nodeStateToString(state);
    }

    /**
     * @brief Check if a node is in a terminal state
     * @param node The node to check
     * @return true if node is in Completed, Failed, or Cancelled state
     */
    bool isTerminal(const NodeHandle& node) const {
        return isTerminalState(getState(node));
    }

    /**
     * @brief Get statistics about current state distribution
     * @param stats Output parameter for statistics
     */
    void getStats(WorkGraphStats& stats) const {
        // Copy each atomic field individually
        stats.totalNodes.store(_stats.totalNodes.load(std::memory_order_relaxed));
        stats.completedNodes.store(_stats.completedNodes.load(std::memory_order_relaxed));
        stats.failedNodes.store(_stats.failedNodes.load(std::memory_order_relaxed));
        stats.cancelledNodes.store(_stats.cancelledNodes.load(std::memory_order_relaxed));
        stats.pendingNodes.store(_stats.pendingNodes.load(std::memory_order_relaxed));
        stats.readyNodes.store(_stats.readyNodes.load(std::memory_order_relaxed));
        stats.scheduledNodes.store(_stats.scheduledNodes.load(std::memory_order_relaxed));
        stats.executingNodes.store(_stats.executingNodes.load(std::memory_order_relaxed));
        stats.memoryUsage.store(_stats.memoryUsage.load(std::memory_order_relaxed));
        stats.totalExecutionTime = _stats.totalExecutionTime;
    }

    /**
     * @brief Reset all statistics
     */
    void resetStats() {
        _stats.totalNodes.store(0, std::memory_order_relaxed);
        _stats.completedNodes.store(0, std::memory_order_relaxed);
        _stats.failedNodes.store(0, std::memory_order_relaxed);
        _stats.cancelledNodes.store(0, std::memory_order_relaxed);
        _stats.pendingNodes.store(0, std::memory_order_relaxed);
        _stats.readyNodes.store(0, std::memory_order_relaxed);
        _stats.scheduledNodes.store(0, std::memory_order_relaxed);
        _stats.executingNodes.store(0, std::memory_order_relaxed);
        _stats.memoryUsage.store(0, std::memory_order_relaxed);
        _stats.totalExecutionTime = {};
    }

    /**
     * @brief Register a node with initial state
     * @param node The node to register
     * @param initialState Initial state (default: Pending)
     */
    void registerNode(const NodeHandle& node, NodeState initialState = NodeState::Pending);

    /**
     * @brief Batch update for multiple nodes
     * @param updates Vector of (node, from, to) tuples
     * @return Number of successful transitions
     */
    size_t batchTransition(const std::vector<std::tuple<NodeHandle, NodeState, NodeState>>& updates);

    /**
     * @brief Get all nodes in a specific state
     * @param state The state to query
     * @param allNodes List of all nodes to check
     * @param output Vector to fill with matching nodes
     */
    void getNodesInState(NodeState state, const std::vector<NodeHandle>& allNodes,
                         std::vector<NodeHandle>& output) const;

    /**
     * @brief Check if any nodes are in non-terminal states
     * @return true if there are pending, ready, scheduled, or executing nodes
     */
    bool hasActiveNodes() const {
        return _stats.pendingNodes.load(std::memory_order_relaxed) > 0 ||
               _stats.readyNodes.load(std::memory_order_relaxed) > 0 ||
               _stats.scheduledNodes.load(std::memory_order_relaxed) > 0 ||
               _stats.executingNodes.load(std::memory_order_relaxed) > 0;
    }

    /**
     * @brief Get memory usage estimate
     * @return Approximate memory usage in bytes
     */
    size_t getMemoryUsage() const {
        return sizeof(*this);
    }

private:
    const WorkGraph* _graph;
    Core::EventBus* _eventBus;

    // Statistics tracking - atomic for lock-free updates
    WorkGraphStats _stats;

    /**
     * @brief Update statistics when state changes
     */
    void updateStats(NodeState oldState, NodeState newState);

    /**
     * @brief Publish state change event if event bus is configured
     */
    void publishStateChange(const NodeHandle& node, NodeState from, NodeState to);
};

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
