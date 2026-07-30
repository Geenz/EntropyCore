/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "WorkGraph.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>

#include "NodeScheduler.h"
#include "NodeStateManager.h"
#include "WorkGraphEvents.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

WorkGraph::WorkGraph(WorkContractGroup* workContractGroup) : WorkGraph(workContractGroup, WorkGraphConfig{}) {}

WorkGraph::WorkGraph(WorkContractGroup* workContractGroup, const WorkGraphConfig& config)
    : Debug::Named("WorkGraph"), _workContractGroup(workContractGroup), _config(config) {
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph constructor called");
    }

    if (!_workContractGroup) {
        throw std::invalid_argument("WorkGraph requires a valid WorkContractGroup");
    }

    // Create event bus if configured
    if (_config.enableEvents) {
        if (_config.sharedEventBus) {
            // Use provided shared event bus (don't own it)
            // Note: We'll need to be careful about lifetime
        } else {
            // Create our own event bus
            _eventBus = std::make_unique<Core::EventBus>();
        }
    }

    // Always create state manager (it's fundamental to correct operation)
    auto* eventBusPtr = _config.enableEvents ? getEventBus() : nullptr;
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", eventBusPtr ? "WorkGraph: Creating state manager with event bus"
                                                         : "WorkGraph: Creating state manager WITHOUT event bus");
    }
    _stateManager = std::make_unique<NodeStateManager>(this, eventBusPtr);

    // Always create scheduler
    NodeScheduler::Config schedulerConfig;
    schedulerConfig.maxDeferredNodes = _config.maxDeferredNodes;
    schedulerConfig.enableBatchScheduling = _config.enableAdvancedScheduling;
    schedulerConfig.enableDebugLogging = _config.enableDebugLogging;
    _scheduler = std::make_unique<NodeScheduler>(_workContractGroup, this,
                                                 _config.enableEvents ? getEventBus() : nullptr, schedulerConfig);

    // Set up safe scheduler callbacks with proper lifetime tracking
    NodeScheduler::Callbacks callbacks;
    callbacks.onNodeExecuting = [this](const NodeHandle& node) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            if (_config.enableDebugLogging) {
                auto* nodeData = _graph.getNodeData(node);
                if (nodeData) {
                    ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node transitioning to Executing, current state: " +
                                                             std::to_string(static_cast<int>(nodeData->state.load())));
                } else {
                    ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node transitioning to Executing");
                }
            }
            // Node could be in Ready or Scheduled state when execution starts
            auto* nodeData = _graph.getNodeData(node);
            if (nodeData) {
                NodeState currentState = nodeData->state.load(std::memory_order_acquire);
                if (currentState == NodeState::Ready || currentState == NodeState::Scheduled) {
                    _stateManager->transitionState(node, currentState, NodeState::Executing);
                }
            }
        }
    };
    callbacks.onNodeCompleted = [this](const NodeHandle& node) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node completed");
            }
            onNodeComplete(node);
        }
    };
    callbacks.onNodeFailed = [this](const NodeHandle& node, const std::exception_ptr& /*ex*/) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_ERROR_CAT("Concurrency", "WorkGraph: Node failed");
            }
            onNodeFailed(node);
        }
    };
    callbacks.onNodeDropped = [this](const NodeHandle& node) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            // Mark the node as failed (dropped is effectively a failure)
            auto* nodeData = _graph.getNodeData(node);
            if (nodeData) {
                // Prevent double-processing
                bool expected = false;
                if (nodeData->completionProcessed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    // Cancelled, not Failed: dropped nodes are Scheduled at drop
                    // time and Scheduled->Failed is not a legal transition (the
                    // attempt silently failed and left the node in Scheduled, a
                    // non-terminal state, forever). The drop is still reported
                    // via _droppedNodes below.
                    _stateManager->transitionState(node, nodeData->state.load(), NodeState::Cancelled);

                    // Increment dropped count and decrement pending count
                    _droppedNodes.fetch_add(1, std::memory_order_relaxed);
                    uint32_t pending = _pendingNodes.fetch_sub(1, std::memory_order_acq_rel) - 1;

                    // Cancel all dependent nodes (like a failed node would)
                    cancelDependents(node);

                    // If all nodes are "done" (completed/failed/dropped), notify waiters
                    if (pending == 0) {
                        std::lock_guard<std::mutex> lock(_waitMutex);
                        _waitCondition.notify_all();
                    }

                    ENTROPY_LOG_ERROR_CAT("WorkGraph", "Node dropped due to deferred queue overflow!");
                }
            }
        }
    };
    callbacks.onNodeYielded = [this](const NodeHandle& node) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node yielded");
            }
            onNodeYielded(node);
        }
    };
    callbacks.onNodeYieldedUntil = [this](const NodeHandle& node, std::chrono::steady_clock::time_point wakeTime) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node yielded until specific time");
            }
            onNodeYieldedUntil(node, wakeTime);
        }
    };
    _scheduler->setCallbacks(callbacks);

    // Register callback for when contract capacity becomes available
    // This allows us to process deferred nodes at the right time
    // We process multiple rounds to keep the pipeline full
    _capacityCallbackIt = _workContractGroup->addOnCapacityAvailable([this]() {
        CallbackGuard guard(this);
        if (_suspended.load(std::memory_order_acquire)) {
            return;  // Suspended: deferred nodes stay deferred until resume()
        }
        if (!_destroyed.load(std::memory_order_acquire) && _scheduler) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Capacity available callback triggered");
            }

            // First, check timed deferrals - wake up any timers/delayed work that's ready
            size_t timedProcessed = _scheduler->processTimedDeferredNodes();
            if (timedProcessed > 0 && _config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT(
                    "Concurrency", "WorkGraph: Processed " + std::to_string(timedProcessed) + " timed deferred nodes");
            }

            // Then process regular deferred nodes multiple times to fill capacity
            // This is important when we have many deferred nodes
            for (size_t i = 0; i < _config.maxDeferredProcessingIterations; i++) {
                size_t processed = _scheduler->processDeferredNodes();
                if (processed == 0) break;  // No more capacity or no more deferred nodes
                if (_config.enableDebugLogging) {
                    ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Capacity callback processed " +
                                                             std::to_string(processed) + " deferred nodes");
                }
            }
        }
    });

    // Register timed deferral callback to avoid dynamic_cast in WorkService
    _timedDeferralCallbackIt = _workContractGroup->addTimedDeferralCallback([this]() { return checkTimedDeferrals(); });

    // Register with debug system (can be disabled via config)
    if (_config.enableDebugRegistration) {
        Debug::DebugRegistry::getInstance().registerObject(this, "WorkGraph");
        auto msg = std::format("Created WorkGraph '{}' with contract group", getName());
        ENTROPY_LOG_DEBUG_CAT("WorkGraph", msg);
    }
}

WorkGraph::~WorkGraph() {
    try {
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT(
                "Concurrency", "WorkGraph destructor starting, pending nodes: " + std::to_string(_pendingNodes.load()));
        }

        // Set destroyed flag to prevent new callbacks
        _destroyed.store(true, std::memory_order_release);

        // Unregister callbacks from WorkContractGroup first
        // This prevents new callbacks from being scheduled
        if (_workContractGroup) {
            _workContractGroup->removeOnCapacityAvailable(_capacityCallbackIt);
            _workContractGroup->removeTimedDeferralCallback(_timedDeferralCallbackIt);
        }

        // Drain any in-flight run before tearing down. Wrapper lambdas live in
        // the contract group's slots and reference this graph and its
        // scheduler; destroying the graph while contracts are scheduled or
        // executing leaves them pointing at freed memory. Use the group's own
        // protocol: stop() blocks new selection, wait() (stopping mode) blocks
        // until executing wrappers finish, then release our remaining
        // contracts so nothing referencing this graph can run later, and
        // resume the group for its other users.
        if (_workContractGroup && _pendingNodes.load(std::memory_order_acquire) > 0) {
            ENTROPY_LOG_WARNING_CAT(
                "WorkGraph", "WorkGraph destroyed with pending nodes; draining in-flight work (wait() first to avoid "
                             "this stall)");
            _workContractGroup->stop();
            _workContractGroup->wait();
            {
                std::unique_lock<std::shared_mutex> lock(_graphMutex);
                for (auto& handle : _nodeHandles) {
                    auto* nodeData = _graph.getNodeData(handle);
                    if (nodeData && nodeData->handle.valid()) {
                        nodeData->handle.unschedule();
                        nodeData->handle.release();
                    }
                }
            }
            _workContractGroup->resume();
        }

        // Wait for all active callbacks to complete
        if (_activeCallbacks.load(std::memory_order_acquire) > 0) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph destructor waiting for callbacks: " +
                                                         std::to_string(_activeCallbacks.load()));
            }
            std::unique_lock<std::mutex> lock(_waitMutex);
            _shutdownCondition.wait(lock, [this]() { return _activeCallbacks.load(std::memory_order_acquire) == 0; });
        }

        // Now safe to proceed with cleanup
        // Unregister from debug system (if registered)
        if (_config.enableDebugRegistration) {
            Debug::DebugRegistry::getInstance().unregisterObject(this);
            auto msg = std::format("Destroyed WorkGraph '{}'", getName());
            ENTROPY_LOG_DEBUG_CAT("WorkGraph", msg);
        }
    } catch (...) {
        // Suppress exceptions in destructor - cannot propagate
    }
}

void WorkGraph::throwIfFrozenLocked(const char* operation) const {
    // Build once, execute many: graph structure is frozen from execute() until
    // reset(). The execution paths (work wrappers, completion cascade,
    // deferred scheduling) walk node storage WITHOUT _graphMutex on the
    // strength of this guarantee - mutation mid-run would let the node vector
    // reallocate under them.
    if (_executionStarted.load(std::memory_order_acquire)) {
        throw std::logic_error(std::format(
            "WorkGraph::{}: structure is frozen while a run is in flight; wait() for completion and reset() first",
            operation));
    }
}

WorkGraph::NodeHandle WorkGraph::addNodeLocked(WorkGraphNode&& node, void* userData, uint32_t pinnedLane) {
    // Caller holds _graphMutex exclusively and has passed the freeze check.
    if (node.executionType == ExecutionType::PinnedThread &&
        pinnedLane >= _workContractGroup->maxPinnedLanes()) {
        // Loud and early: a node pinned to a lane nothing pumps would strand
        // wait() at execution time, far from the mistake.
        throw std::invalid_argument(std::format("WorkGraph::addNode: pinned lane {} out of range (maxPinnedLanes={})",
                                                pinnedLane, _workContractGroup->maxPinnedLanes()));
    }
    node.userData = userData;
    node.pinnedLane = pinnedLane;

    // Add to graph and track as pending
    auto handle = _graph.addNode(std::move(node));
    _pendingNodes.fetch_add(1, std::memory_order_relaxed);

    // Cache the handle for access later
    _nodeHandles.push_back(handle);

    // Register with state manager
    _stateManager->registerNode(handle, NodeState::Pending);

    // Publish event if enabled
    if (auto* eventBus = getEventBus()) {
        eventBus->publish(NodeAddedEvent(this, handle));
    }

    return handle;
}

WorkGraph::NodeHandle WorkGraph::addNode(std::function<void()> work, const std::string& name, void* userData,
                                         ExecutionType executionType, uint32_t pinnedLane) {
    std::unique_lock<std::shared_mutex> lock(_graphMutex);
    throwIfFrozenLocked("addNode");
    return addNodeLocked(WorkGraphNode(std::move(work), name, executionType), userData, pinnedLane);
}

WorkGraph::NodeHandle WorkGraph::addYieldableNode(YieldableWorkFunction work, const std::string& name, void* userData,
                                                  ExecutionType executionType, std::optional<uint32_t> maxReschedules,
                                                  uint32_t pinnedLane) {
    NodeHandle handle;
    {
        std::unique_lock<std::shared_mutex> lock(_graphMutex);
        throwIfFrozenLocked("addYieldableNode");

        WorkGraphNode node(std::move(work), name, executionType);
        node.maxReschedules = maxReschedules;
        handle = addNodeLocked(std::move(node), userData, pinnedLane);
    }

    if (_config.enableDebugLogging) {
        auto msg = std::format("Added yieldable node '{}' with max reschedules: {}", name,
                               maxReschedules.has_value() ? std::to_string(*maxReschedules) : "unlimited");
        ENTROPY_LOG_DEBUG_CAT("WorkGraph", msg);
    }

    return handle;
}

void WorkGraph::addDependency(NodeHandle from, const NodeHandle& to) {
    std::unique_lock<std::shared_mutex> lock(_graphMutex);
    throwIfFrozenLocked("addDependency");
    addDependencyLocked(std::move(from), to);
}

void WorkGraph::addDependencyLocked(NodeHandle from, const NodeHandle& to) {
    // Caller holds _graphMutex exclusively and has passed the freeze check.
    // Add edge in the DAG (this checks for cycles)
    _graph.addEdge(std::move(from), to);

    // Increment dependency count for the target node
    incrementDependencies(to);
}

void WorkGraph::reset() {
    std::unique_lock<std::shared_mutex> lock(_graphMutex);

    // reset() re-arms a COMPLETED graph for another run; resetting node state
    // while wrappers are still executing would corrupt the run in flight.
    if (_executionStarted.load(std::memory_order_acquire) && _pendingNodes.load(std::memory_order_acquire) > 0) {
        throw std::logic_error("WorkGraph::reset: cannot reset while a run is in flight; wait() first");
    }

    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::reset() - resetting execution state for " +
                                                 std::to_string(_nodeHandles.size()) + " nodes");
    }

    // Reset execution flag
    _executionStarted.store(false, std::memory_order_release);

    // Reset counters
    _pendingNodes.store(static_cast<uint32_t>(_nodeHandles.size()), std::memory_order_release);
    _completedNodes.store(0, std::memory_order_release);
    _failedNodes.store(0, std::memory_order_release);
    _droppedNodes.store(0, std::memory_order_release);

    // Reset each node's state
    for (auto& handle : _nodeHandles) {
        auto* nodeData = _graph.getNodeData(handle);
        if (nodeData) {
            // Reset to Pending state
            nodeData->state.store(NodeState::Pending, std::memory_order_release);
            nodeData->completionProcessed.store(false, std::memory_order_release);
            nodeData->failedParentCount.store(0, std::memory_order_release);
            nodeData->rescheduleCount.store(0, std::memory_order_release);

            // Reset pending dependencies by counting incoming edges
            nodeData->pendingDependencies.store(0, std::memory_order_release);
        }

        // Update state manager
        if (_stateManager) {
            _stateManager->registerNode(handle, NodeState::Pending);
        }
    }

    // Restore dependency counts from edge structure
    for (auto& handle : _nodeHandles) {
        auto children = _graph.getChildren(handle);
        for (auto& child : children) {
            auto* childData = _graph.getNodeData(child);
            if (childData) {
                childData->pendingDependencies.fetch_add(1, std::memory_order_acq_rel);
            }
        }
    }

    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::reset() complete");
    }
}

void WorkGraph::clear() {
    std::unique_lock<std::shared_mutex> lock(_graphMutex);

    // Destroying node storage while wrappers are executing is a use-after-free.
    if (_executionStarted.load(std::memory_order_acquire) && _pendingNodes.load(std::memory_order_acquire) > 0) {
        throw std::logic_error("WorkGraph::clear: cannot clear while a run is in flight; wait() first");
    }

    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency",
                              "WorkGraph::clear() - removing all " + std::to_string(_nodeHandles.size()) + " nodes");
    }

    // Reset execution flag
    _executionStarted.store(false, std::memory_order_release);

    // Reset counters
    _pendingNodes.store(0, std::memory_order_release);
    _completedNodes.store(0, std::memory_order_release);
    _failedNodes.store(0, std::memory_order_release);
    _droppedNodes.store(0, std::memory_order_release);

    // Clear the node handles cache
    _nodeHandles.clear();

    // Clear the underlying DAG
    _graph.clear();

    // Reset state manager (it will be repopulated as nodes are added)
    // Note: We don't need to explicitly clear it - registerNode overwrites

    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::clear() complete");
    }
}

void WorkGraph::incrementDependencies(const NodeHandle& node) {
    if (auto* nodeData = _graph.getNodeData(node)) {
        nodeData->pendingDependencies.fetch_add(1, std::memory_order_acq_rel);
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node dependencies incremented");
        }
    }
}

size_t WorkGraph::scheduleRoots() {
    std::vector<NodeHandle> toSchedule;
    {
        std::shared_lock<std::shared_mutex> lock(_graphMutex);
        toSchedule = collectReadyRootsLocked();
    }

    // Schedule outside the lock: see collectReadyRootsLocked().
    size_t rootCount = 0;
    for (auto& handle : toSchedule) {
        if (_scheduler->scheduleNode(handle)) {
            rootCount++;
        } else if (_config.enableDebugLogging) {
            ENTROPY_LOG_WARNING_CAT("Concurrency", "WorkGraph: Failed to schedule root node");
        }
    }

    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Root node scheduling complete, scheduled " +
                                                 std::to_string(rootCount) + " roots");
    }

    return rootCount;
}

std::vector<WorkGraph::NodeHandle> WorkGraph::collectReadyRootsLocked() {
    std::vector<NodeHandle> toSchedule;

    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency",
                              "WorkGraph: Checking " + std::to_string(_nodeHandles.size()) + " nodes for roots");
    }

    bool suspended = _suspended.load(std::memory_order_acquire);

    // Check all cached handles to find roots (nodes ready to execute)
    for (auto& handle : _nodeHandles) {
        if (!isHandleValid(handle)) continue;

        auto* nodeData = _graph.getNodeData(handle);
        if (!nodeData || nodeData->pendingDependencies.load() != 0) continue;

        // Try to transition to ready state through state manager
        if (_stateManager->transitionState(handle, NodeState::Pending, NodeState::Ready)) {
            if (suspended) {
                // Leave in Ready; resume() schedules it
                continue;
            }
            // Now transition to scheduled before actually scheduling
            if (_stateManager->transitionState(handle, NodeState::Ready, NodeState::Scheduled)) {
                toSchedule.push_back(handle);
            }
        } else if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Failed to transition root node to Ready state");
        }
    }

    return toSchedule;
}

void WorkGraph::suspend() {
    _suspended.store(true, std::memory_order_release);

    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("WorkGraph", "Graph suspended - no new nodes will be scheduled");
    }
}

void WorkGraph::resume() {
    bool wasSuspended = _suspended.exchange(false, std::memory_order_acq_rel);

    if (wasSuspended) {
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("WorkGraph", "Graph resumed - checking for ready nodes");
        }

        // Process any deferred nodes that accumulated while suspended
        if (_scheduler) {
            size_t processed = _scheduler->processDeferredNodes();
            if (processed > 0 && _config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("WorkGraph",
                                      "Processed " + std::to_string(processed) + " deferred nodes after resume");
            }
        }

        // Check if any nodes became ready while we were suspended, then
        // schedule them AFTER dropping the lock: scheduleNode's drop callback
        // re-enters _graphMutex via cancelDependents.
        std::vector<NodeHandle> toSchedule;
        {
            std::shared_lock<std::shared_mutex> lock(_graphMutex);
            for (const auto& handle : _nodeHandles) {
                auto* nodeData = _graph.getNodeData(handle);
                if (nodeData && nodeData->state.load() == NodeState::Ready) {
                    // Try to transition to scheduled
                    if (_stateManager->transitionState(handle, NodeState::Ready, NodeState::Scheduled)) {
                        toSchedule.push_back(handle);
                    }
                }
            }
        }
        for (auto& handle : toSchedule) {
            _scheduler->scheduleNode(handle);
        }
    }
}

void WorkGraph::execute() {
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_INFO_CAT("Concurrency", "WorkGraph::execute() starting");
    }

    std::vector<NodeHandle> toSchedule;
    {
        // Exclusive lock: freezes the structure (mutators throw once
        // _executionStarted is set) and collects the roots atomically with it.
        std::unique_lock<std::shared_mutex> lock(_graphMutex);

        bool expected = false;
        if (!_executionStarted.compare_exchange_strong(expected, true)) {
            throw std::runtime_error("WorkGraph execution already started");
        }

        toSchedule = collectReadyRootsLocked();
    }

    // Cycle check before scheduling: pending nodes but no schedulable root
    // (and not suspended, which legitimately leaves roots in Ready).
    if (toSchedule.empty() && !_suspended.load(std::memory_order_acquire) && getPendingCount() > 0) {
        auto msg = std::format("ERROR: No roots found. Pending count: {}, node count: {}", getPendingCount(),
                               _nodeHandles.size());
        ENTROPY_LOG_ERROR_CAT("WorkGraph", msg);
        throw std::runtime_error("WorkGraph has no root nodes but has pending work - possible cycle?");
    }

    // Schedule outside the lock: scheduleNode can synchronously invoke the drop
    // callback (cancelDependents re-enters _graphMutex), and scheduled work can
    // start completing on worker threads immediately.
    size_t roots = 0;
    for (auto& handle : toSchedule) {
        if (_scheduler->scheduleNode(handle)) {
            roots++;
        }
    }

    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::execute() scheduled " + std::to_string(roots) + " root nodes");
    }

    // After unlocking, process any deferred nodes
    if (_scheduler) {
        size_t deferred = _scheduler->getDeferredCount();
        if (_config.enableDebugLogging && deferred > 0) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::execute() has " + std::to_string(deferred) +
                                                     " deferred nodes after initial scheduling");
        }
        if (deferred > 0) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::execute() calling processDeferredNodes");
            }
            size_t processed = _scheduler->processDeferredNodes();
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT(
                    "Concurrency", "WorkGraph::execute() processDeferredNodes returned " + std::to_string(processed));
            }
        }
    }

    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::execute() completed");
    }
}

bool WorkGraph::scheduleNode(const NodeHandle& node) {
    // Check if suspended
    if (_suspended.load(std::memory_order_acquire)) {
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::scheduleNode() - graph suspended, deferring node");
        }
        // Don't schedule while suspended
        return false;
    }

    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::scheduleNode() called");
    }
    // Always delegate to the scheduler component
    bool result = _scheduler->scheduleNode(node);
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::scheduleNode() completed");
    }
    return result;
}

void WorkGraph::onNodeComplete(const NodeHandle& node) {
    std::vector<NodeHandle> toSchedule;
    {
        // ONE shared scope covers the completion transition AND the child
        // dependency decrements. This is load-bearing for two reasons:
        // 1. Node storage can reallocate under addNode's exclusive lock, so
        //    every nodeData/childData dereference must sit inside a scope.
        // 2. addDependencyLocked's terminal-parent check relies on its
        //    exclusive section ordering entirely before this scope (edge seen
        //    here, its increment matched by our decrement) or entirely after
        //    (parent observed Completed, increment skipped). Splitting the
        //    transition and the decrements into separate scopes lets an edge
        //    slip between them and the child's count underflows.
        std::shared_lock<std::shared_mutex> lock(_graphMutex);

        auto* nodeData = _graph.getNodeData(node);
        if (!nodeData) return;

        // Prevent double-processing using atomic flag
        bool expected = false;
        if (!nodeData->completionProcessed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_WARNING_CAT("Concurrency", "WorkGraph: Node already processed completion");
            }
            return;  // Already processed
        }

        // Transition state through state manager
        _stateManager->transitionState(node, NodeState::Executing, NodeState::Completed);

        // Decrement child dependency counts; collect newly-ready children
        auto children = this->getChildren(node);
        bool suspended = _suspended.load(std::memory_order_acquire);
        for (auto& child : children) {
            auto* childData = _graph.getNodeData(child);
            if (!childData) continue;

            // Skip if child is cancelled
            if (childData->state.load(std::memory_order_acquire) == NodeState::Cancelled) {
                continue;
            }

            uint32_t remaining = childData->pendingDependencies.fetch_sub(1, std::memory_order_acq_rel) - 1;

            if (remaining == 0 && childData->failedParentCount.load(std::memory_order_acquire) == 0) {
                if (_stateManager->transitionState(child, NodeState::Pending, NodeState::Ready)) {
                    if (suspended) {
                        // Leave the child in Ready; resume() schedules it.
                        // (Previously the completion cascade ignored suspension
                        // and kept scheduling behind suspend()'s back.)
                        continue;
                    }
                    if (_stateManager->transitionState(child, NodeState::Ready, NodeState::Scheduled)) {
                        toSchedule.push_back(child);
                    }
                }
            }
        }
    }

    // Update counters
    _completedNodes.fetch_add(1, std::memory_order_relaxed);
    uint32_t pending = _pendingNodes.fetch_sub(1, std::memory_order_acq_rel) - 1;

    // If all nodes are complete, notify waiters
    if (pending == 0) {
        std::lock_guard<std::mutex> lock(_waitMutex);
        _waitCondition.notify_all();
    }

    // Call completion callback if set (no locks held)
    if (_onNodeComplete) {
        _onNodeComplete(node);
    }

    // Schedule children outside the lock: scheduleNode's drop callback
    // re-enters _graphMutex via cancelDependents.
    for (auto& child : toSchedule) {
        _scheduler->scheduleNode(child);
    }

    // Note: Processing of deferred nodes is now handled via the
    // onCapacityAvailable callback from WorkContractGroup, which
    // is called after contracts are actually freed.
}

WorkGraph::WaitResult WorkGraph::wait() {
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::wait() called");
    }

    // Check if already complete before waiting
    if (_pendingNodes.load(std::memory_order_acquire) == 0) {
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::wait() - already complete, no need to wait");
        }
        // Already complete, prepare result immediately
        WaitResult result;
        result.completedCount = _completedNodes.load(std::memory_order_acquire);
        result.failedCount = _failedNodes.load(std::memory_order_acquire);
        result.droppedCount = _droppedNodes.load(std::memory_order_acquire);
        result.allCompleted = (result.failedCount == 0 && result.droppedCount == 0);
        return result;
    }

    // Use condition variable for waiting instead of busy-wait
    std::unique_lock<std::mutex> lock(_waitMutex);
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency",
                              "WorkGraph::wait() waiting for pending nodes: " + std::to_string(_pendingNodes.load()));
    }
    _waitCondition.wait(lock, [this]() {
        auto pending = _pendingNodes.load(std::memory_order_acquire);
        if (_config.enableDebugLogging && pending > 0) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency",
                                  "WorkGraph::wait() still waiting, pending: " + std::to_string(pending));
        }
        return pending == 0;
    });

    // Prepare result
    WaitResult result;
    result.completedCount = _completedNodes.load(std::memory_order_acquire);
    result.failedCount = _failedNodes.load(std::memory_order_acquire);
    result.droppedCount = _droppedNodes.load(std::memory_order_acquire);
    result.allCompleted = (result.failedCount == 0 && result.droppedCount == 0);

    // Log warning if nodes were dropped
    if (result.droppedCount > 0) {
        auto msg = std::format("WorkGraph::wait() - {} nodes were dropped due to deferred queue overflow!",
                               result.droppedCount);
        ENTROPY_LOG_WARNING_CAT("WorkGraph", msg);
    }

    return result;
}

bool WorkGraph::isComplete() const {
    auto pending = _pendingNodes.load(std::memory_order_acquire);
    if (_config.enableDebugLogging && pending > 0) {
        auto completed = _completedNodes.load(std::memory_order_acquire);
        auto failed = _failedNodes.load(std::memory_order_acquire);
        auto dropped = _droppedNodes.load(std::memory_order_acquire);
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::isComplete() - pending: " + std::to_string(pending) +
                                                 ", completed: " + std::to_string(completed) + ", failed: " +
                                                 std::to_string(failed) + ", dropped: " + std::to_string(dropped));
    }
    return pending == 0;
}

size_t WorkGraph::processDeferredNodes() {
    // Delegate to scheduler to process any deferred nodes
    if (_scheduler) {
        return _scheduler->processDeferredNodes();
    }
    return 0;
}

size_t WorkGraph::checkTimedDeferrals() {
    // Delegate to scheduler to process timed deferred nodes
    if (_scheduler) {
        return _scheduler->processTimedDeferredNodes();
    }
    return 0;
}

WorkGraph::NodeHandle WorkGraph::addContinuation(const std::vector<NodeHandle>& parents, std::function<void()> work,
                                                 const std::string& name, ExecutionType executionType,
                                                 uint32_t pinnedLane) {
    // Node and edges under ONE exclusive section: a concurrent execute() between
    // separate addNode/addDependency acquisitions could freeze the graph and
    // schedule the still-edgeless continuation as a root, running it before its
    // parents.
    std::unique_lock<std::shared_mutex> lock(_graphMutex);
    throwIfFrozenLocked("addContinuation");

    auto continuation = addNodeLocked(WorkGraphNode(std::move(work), name, executionType), nullptr, pinnedLane);
    for (const auto& parent : parents) {
        addDependencyLocked(parent, continuation);
    }

    return continuation;
}

void WorkGraph::onNodeFailed(const NodeHandle& node) {
    {
        // Shared scope for the nodeData dereference (storage can reallocate
        // under addNode's exclusive lock) and to order the terminal transition
        // against addDependencyLocked's terminal-parent check.
        std::shared_lock<std::shared_mutex> lock(_graphMutex);
        auto* nodeData = _graph.getNodeData(node);
        if (!nodeData) return;

        // Prevent double-processing
        bool expected = false;
        if (!nodeData->completionProcessed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;  // Already processed
        }

        // Transition state through state manager
        _stateManager->transitionState(node, NodeState::Executing, NodeState::Failed);
    }

    // Update counters
    _failedNodes.fetch_add(1, std::memory_order_relaxed);
    uint32_t pending = _pendingNodes.fetch_sub(1, std::memory_order_acq_rel) - 1;

    // If all nodes are complete, notify waiters
    if (pending == 0) {
        std::lock_guard<std::mutex> lock(_waitMutex);
        _waitCondition.notify_all();
    }

    // Cancel all dependent nodes (takes its own lock scopes)
    cancelDependents(node);
}

void WorkGraph::onNodeYielded(const NodeHandle& node) {
    auto* nodeData = _graph.getNodeData(node);
    if (!nodeData) return;

    // Increment reschedule count
    uint32_t rescheduleCount = nodeData->rescheduleCount.fetch_add(1, std::memory_order_relaxed);

    if (_config.enableDebugLogging) {
        auto msg = std::format("Node '{}' yielded (reschedule count: {})", nodeData->name, rescheduleCount + 1);
        ENTROPY_LOG_DEBUG_CAT("WorkGraph", msg);
    }

    // Check reschedule limit
    if (nodeData->maxReschedules && rescheduleCount >= *nodeData->maxReschedules) {
        if (_config.enableDebugLogging) {
            auto msg = std::format("Node '{}' reached max reschedule limit ({}), completing", nodeData->name,
                                   *nodeData->maxReschedules);
            ENTROPY_LOG_WARNING_CAT("WorkGraph", msg);
        }

        // Hit limit - treat as completed
        onNodeComplete(node);
        return;
    }

    // Transition to Yielded state
    if (_stateManager) {
        _stateManager->transitionState(node, NodeState::Executing, NodeState::Yielded);
    }

    // Reschedule the node immediately
    rescheduleYieldedNode(node);
}

void WorkGraph::onNodeYieldedUntil(const NodeHandle& node, std::chrono::steady_clock::time_point wakeTime) {
    auto* nodeData = _graph.getNodeData(node);
    if (!nodeData) return;

    // Increment reschedule count
    uint32_t rescheduleCount = nodeData->rescheduleCount.fetch_add(1, std::memory_order_relaxed);

    if (_config.enableDebugLogging) {
        auto now = std::chrono::steady_clock::now();
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(wakeTime - now);
        auto msg = std::format("Node '{}' yielded until wake time (delay: {}ms, reschedule count: {})", nodeData->name,
                               delay.count(), rescheduleCount + 1);
        ENTROPY_LOG_DEBUG_CAT("WorkGraph", msg);
    }

    // Check reschedule limit
    if (nodeData->maxReschedules && rescheduleCount >= *nodeData->maxReschedules) {
        if (_config.enableDebugLogging) {
            auto msg = std::format("Node '{}' reached max reschedule limit ({}), completing", nodeData->name,
                                   *nodeData->maxReschedules);
            ENTROPY_LOG_WARNING_CAT("WorkGraph", msg);
        }

        // Hit limit - treat as completed
        onNodeComplete(node);
        return;
    }

    // Transition to Yielded state
    if (_stateManager) {
        _stateManager->transitionState(node, NodeState::Executing, NodeState::Yielded);
    }

    // Defer until wake time (not immediate reschedule!)
    if (_scheduler) {
        _scheduler->deferNodeUntil(node, wakeTime);
    }
}

void WorkGraph::rescheduleYieldedNode(const NodeHandle& node) {
    auto* nodeData = _graph.getNodeData(node);
    if (!nodeData) return;

    if (_config.enableDebugLogging) {
        auto msg = std::format("Rescheduling yielded node '{}'", nodeData->name);
        ENTROPY_LOG_DEBUG_CAT("WorkGraph", msg);
    }

    // Clear the completion processed flag so it can run again
    nodeData->completionProcessed.store(false, std::memory_order_release);

    // Transition from Yielded to Ready
    if (_stateManager) {
        if (_stateManager->transitionState(node, NodeState::Yielded, NodeState::Ready)) {
            // If suspended, don't try to schedule - leave in Ready state
            if (_suspended.load(std::memory_order_acquire)) {
                if (_config.enableDebugLogging) {
                    ENTROPY_LOG_DEBUG_CAT("WorkGraph", "Graph suspended - yielded node left in Ready state");
                }
                return;
            }

            // Transition to scheduled
            if (_stateManager->transitionState(node, NodeState::Ready, NodeState::Scheduled)) {
                // Schedule with the scheduler
                if (!_scheduler->scheduleNode(node)) {
                    // Failed to schedule - might be deferred
                    if (_config.enableDebugLogging) {
                        ENTROPY_LOG_WARNING_CAT("WorkGraph", "Failed to reschedule yielded node");
                    }
                }
            }
        }
    }
}

void WorkGraph::onNodeCancelled(const NodeHandle& node) {
    auto* nodeData = _graph.getNodeData(node);
    if (!nodeData) return;

    // Prevent double-processing
    bool expected = false;
    if (!nodeData->completionProcessed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // Already processed
    }

    // Transition state through state manager (from whatever state to cancelled)
    NodeState currentState = nodeData->state.load(std::memory_order_acquire);
    _stateManager->transitionState(node, currentState, NodeState::Cancelled);

    // CRITICAL FIX: Decrement pending count for cancelled nodes
    uint32_t pending = _pendingNodes.fetch_sub(1, std::memory_order_acq_rel) - 1;

    // If all nodes are complete, notify waiters
    if (pending == 0) {
        std::lock_guard<std::mutex> lock(_waitMutex);
        _waitCondition.notify_all();
    }

    // Cancel all dependent nodes
    cancelDependents(node);
}

void WorkGraph::cancelDependents(const NodeHandle& failedNode) {
    std::vector<NodeHandle> nodesToCancel;

    {
        std::shared_lock<std::shared_mutex> lock(_graphMutex);

        // Get all children of the failed node
        auto children = this->getChildren(failedNode);

        for (auto& child : children) {
            auto* childData = _graph.getNodeData(child);
            if (!childData) continue;

            // Increment failed parent count
            childData->failedParentCount.fetch_add(1, std::memory_order_acq_rel);

            // If not already in terminal state, add to cancellation list
            NodeState childState = childData->state.load(std::memory_order_acquire);
            if (!isTerminalState(childState)) {
                nodesToCancel.push_back(child);
            }
        }
    }

    // Cancel nodes outside the lock
    for (auto& node : nodesToCancel) {
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Cancelling dependent node due to failed parent");
        }
        onNodeCancelled(node);
    }
}

Core::EventBus* WorkGraph::getEventBus() {
    // Create event bus on demand if configured but not yet created
    if (_config.enableEvents && !_eventBus && !_config.sharedEventBus) {
        _eventBus = std::make_unique<Core::EventBus>();
    }

    // Return shared event bus if configured
    if (_config.sharedEventBus) {
        return _config.sharedEventBus.get();
    }

    return _eventBus.get();
}

WorkGraphStats::Snapshot WorkGraph::getStats() const {
    WorkGraphStats stats;
    _stateManager->getStats(stats);
    return stats.toSnapshot();
}

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
