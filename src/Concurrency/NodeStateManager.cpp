/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "NodeStateManager.h"

#include <format>

#include "../Logging/Logger.h"
#include "WorkGraph.h"
#include "WorkGraphEvents.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

bool NodeStateManager::transitionState(NodeHandle node, NodeState from, NodeState to) {
    // Validate transition
    if (!canTransition(from, to)) {
        // Log warning for invalid state transition attempts
        auto* dag0 = node.handleOwnerAs<Graph::DirectedAcyclicGraph<WorkGraphNode>>();
        auto* nodeData = dag0 ? dag0->getNodeData(node) : nullptr;
        if (nodeData) {
            auto msg = std::format("Invalid state transition attempted: {} -> {} for node: {}", getStateName(from),
                                   getStateName(to), nodeData->name);
            ENTROPY_LOG_WARNING_CAT("NodeStateManager", msg);
        }
        return false;
    }

    // Get node data for atomic state update
    auto* dag = node.handleOwnerAs<Graph::DirectedAcyclicGraph<WorkGraphNode>>();
    auto* nodeData = dag ? dag->getNodeData(node) : nullptr;
    if (!nodeData) {
        return false;
    }

    // Attempt atomic state transition
    NodeState expected = from;
    if (!nodeData->state.compare_exchange_strong(expected, to, std::memory_order_acq_rel)) {
        return false;  // Current state didn't match 'from'
    }

    // State is tracked directly in the node's atomic field

    // Update statistics
    updateStats(from, to);

    // Publish event if event bus is configured
    publishStateChange(node, from, to);

    return true;
}

void NodeStateManager::forceState(NodeHandle node, NodeState to) {
    auto* dag = node.handleOwnerAs<Graph::DirectedAcyclicGraph<WorkGraphNode>>();
    auto* nodeData = dag ? dag->getNodeData(node) : nullptr;
    if (!nodeData) {
        return;
    }

    NodeState from = nodeData->state.exchange(to, std::memory_order_acq_rel);

    // State is tracked directly in the node's atomic field

    // Update statistics
    updateStats(from, to);

    // Publish event
    publishStateChange(node, from, to);
}

NodeState NodeStateManager::getState(NodeHandle node) const {
    auto* dag = node.handleOwnerAs<Graph::DirectedAcyclicGraph<WorkGraphNode>>();
    auto* nodeData = dag ? dag->getNodeData(node) : nullptr;
    if (!nodeData) {
        return NodeState::Pending;
    }

    return nodeData->state.load(std::memory_order_acquire);
}

void NodeStateManager::registerNode(NodeHandle node, NodeState initialState) {
    auto* dag = node.handleOwnerAs<Graph::DirectedAcyclicGraph<WorkGraphNode>>();
    auto* nodeData = dag ? dag->getNodeData(node) : nullptr;
    if (!nodeData) {
        return;
    }

    // Set initial state
    nodeData->state.store(initialState, std::memory_order_release);

    // State is tracked directly in the node's atomic field

    // Update stats atomically
    _stats.totalNodes.fetch_add(1, std::memory_order_relaxed);

    switch (initialState) {
        case NodeState::Pending:
            _stats.pendingNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Ready:
            _stats.readyNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Scheduled:
            _stats.scheduledNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Executing:
            _stats.executingNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Completed:
            _stats.completedNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Failed:
            _stats.failedNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Cancelled:
            _stats.cancelledNodes.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

size_t NodeStateManager::batchTransition(const std::vector<std::tuple<NodeHandle, NodeState, NodeState>>& updates) {
    size_t successCount = 0;

    for (const auto& [node, from, to] : updates) {
        if (transitionState(node, from, to)) {
            successCount++;
        }
    }

    return successCount;
}

void NodeStateManager::getNodesInState(NodeState state, const std::vector<NodeHandle>& allNodes,
                                       std::vector<NodeHandle>& output) const {
    output.clear();

    // Check each node's atomic state directly
    for (const auto& node : allNodes) {
        auto* dagN = node.handleOwnerAs<Graph::DirectedAcyclicGraph<WorkGraphNode>>();
        auto* nodeData = dagN ? dagN->getNodeData(node) : nullptr;
        if (nodeData && nodeData->state.load(std::memory_order_acquire) == state) {
            output.push_back(node);
        }
    }
}

void NodeStateManager::updateStats(NodeState oldState, NodeState newState) {
    // Atomically decrement old state counter
    switch (oldState) {
        case NodeState::Pending:
            if (_stats.pendingNodes.load(std::memory_order_relaxed) > 0)
                _stats.pendingNodes.fetch_sub(1, std::memory_order_relaxed);
            break;
        case NodeState::Ready:
            if (_stats.readyNodes.load(std::memory_order_relaxed) > 0)
                _stats.readyNodes.fetch_sub(1, std::memory_order_relaxed);
            break;
        case NodeState::Scheduled:
            if (_stats.scheduledNodes.load(std::memory_order_relaxed) > 0)
                _stats.scheduledNodes.fetch_sub(1, std::memory_order_relaxed);
            break;
        case NodeState::Executing:
            if (_stats.executingNodes.load(std::memory_order_relaxed) > 0)
                _stats.executingNodes.fetch_sub(1, std::memory_order_relaxed);
            break;
        case NodeState::Completed:
            if (_stats.completedNodes.load(std::memory_order_relaxed) > 0)
                _stats.completedNodes.fetch_sub(1, std::memory_order_relaxed);
            break;
        case NodeState::Failed:
            if (_stats.failedNodes.load(std::memory_order_relaxed) > 0)
                _stats.failedNodes.fetch_sub(1, std::memory_order_relaxed);
            break;
        case NodeState::Cancelled:
            if (_stats.cancelledNodes.load(std::memory_order_relaxed) > 0)
                _stats.cancelledNodes.fetch_sub(1, std::memory_order_relaxed);
            break;
    }

    // Atomically increment new state counter
    switch (newState) {
        case NodeState::Pending:
            _stats.pendingNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Ready:
            _stats.readyNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Scheduled:
            _stats.scheduledNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Executing:
            _stats.executingNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Completed:
            _stats.completedNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Failed:
            _stats.failedNodes.fetch_add(1, std::memory_order_relaxed);
            break;
        case NodeState::Cancelled:
            _stats.cancelledNodes.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

void NodeStateManager::publishStateChange(NodeHandle node, NodeState from, NodeState to) {
    if (!_eventBus) {
        return;  // No event bus configured
    }

    // Publish generic state change event
    _eventBus->publish(NodeStateChangedEvent(_graph, node, from, to));

    // Publish specific events for important transitions
    switch (to) {
        case NodeState::Ready:
            _eventBus->publish(NodeReadyEvent(_graph, node));
            break;

        case NodeState::Scheduled:
            // Note: NodeScheduler also publishes this event, so we might get duplicates
            _eventBus->publish(NodeScheduledEvent(_graph, node));
            break;

        case NodeState::Executing:
            _eventBus->publish(NodeExecutingEvent(_graph, node));
            break;

        case NodeState::Completed:
            _eventBus->publish(NodeCompletedEvent(_graph, node));
            break;

        case NodeState::Failed:
            _eventBus->publish(NodeFailedEvent(_graph, node));
            break;

        case NodeState::Cancelled:
            _eventBus->publish(NodeCancelledEvent(_graph, node));
            break;

        default:
            break;
    }
}

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
