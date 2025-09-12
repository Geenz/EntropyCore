/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "WorkGraph.h"
#include "NodeStateManager.h"
#include "NodeScheduler.h"
#include "WorkGraphEvents.h"
#include <algorithm>
#include <thread>
#include <chrono>
#include <format>

namespace EntropyEngine {
namespace Core {
namespace Concurrency {

WorkGraph::WorkGraph(WorkContractGroup* workContractGroup)
    : WorkGraph(workContractGroup, WorkGraphConfig{}) {
}

WorkGraph::WorkGraph(WorkContractGroup* workContractGroup, const WorkGraphConfig& config)
    : Debug::Named("WorkGraph")
    , _workContractGroup(workContractGroup)
    , _config(config) {
    ENTROPY_PROFILE_ZONE();
    
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
        ENTROPY_LOG_DEBUG_CAT("Concurrency", eventBusPtr ? "WorkGraph: Creating state manager with event bus" : "WorkGraph: Creating state manager WITHOUT event bus");
    }
    _stateManager = std::make_unique<NodeStateManager>(
        this, 
        eventBusPtr
    );
    
    // Always create scheduler
    NodeScheduler::Config schedulerConfig;
    schedulerConfig.maxDeferredNodes = _config.maxDeferredNodes;
    schedulerConfig.enableBatchScheduling = _config.enableAdvancedScheduling;
    schedulerConfig.enableDebugLogging = _config.enableDebugLogging;
    _scheduler = std::make_unique<NodeScheduler>(
        _workContractGroup,
        this,
        _config.enableEvents ? getEventBus() : nullptr,
        schedulerConfig
    );
    
    // Set up safe scheduler callbacks with proper lifetime tracking
    NodeScheduler::Callbacks callbacks;
    callbacks.onNodeExecuting = [this](NodeHandle node) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            if (_config.enableDebugLogging) {
                auto* nodeData = node.getData();
                if (nodeData) {
                    ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node transitioning to Executing, current state: " + std::to_string(static_cast<int>(nodeData->state.load())));
                } else {
                    ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node transitioning to Executing");
                }
            }
            // Node could be in Ready or Scheduled state when execution starts
            auto* nodeData = node.getData();
            if (nodeData) {
                NodeState currentState = nodeData->state.load(std::memory_order_acquire);
                if (currentState == NodeState::Ready || currentState == NodeState::Scheduled) {
                    _stateManager->transitionState(node, currentState, NodeState::Executing);
                }
            }
        }
    };
    callbacks.onNodeCompleted = [this](NodeHandle node) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node completed");
            }
            onNodeComplete(node);
        }
    };
    callbacks.onNodeFailed = [this](NodeHandle node, std::exception_ptr /*ex*/) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_ERROR_CAT("Concurrency", "WorkGraph: Node failed");
            }
            onNodeFailed(node);
        }
    };
    callbacks.onNodeDropped = [this](NodeHandle node) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            // Mark the node as failed (dropped is effectively a failure)
            auto* nodeData = node.getData();
            if (nodeData) {
                // Prevent double-processing
                bool expected = false;
                if (nodeData->completionProcessed.compare_exchange_strong(expected, true, 
                                                                          std::memory_order_acq_rel)) {
                    // Transition to failed state
                    _stateManager->transitionState(node, nodeData->state.load(), NodeState::Failed);
                    
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
    callbacks.onNodeYielded = [this](NodeHandle node) {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire)) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node yielded");
            }
            onNodeYielded(node);
        }
    };
    _scheduler->setCallbacks(callbacks);
    
    // Register callback for when contract capacity becomes available
    // This allows us to process deferred nodes at the right time
    // We process multiple rounds to keep the pipeline full
    _capacityCallbackIt = _workContractGroup->addOnCapacityAvailable([this]() {
        CallbackGuard guard(this);
        if (!_destroyed.load(std::memory_order_acquire) && _scheduler) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Capacity available callback triggered");
            }
            // Try to process deferred nodes multiple times to fill capacity
            // This is important when we have many deferred nodes
            for (size_t i = 0; i < _config.maxDeferredProcessingIterations; i++) {
                size_t processed = _scheduler->processDeferredNodes();
                if (processed == 0) break; // No more capacity or no more deferred nodes
                if (_config.enableDebugLogging) {
                    ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Capacity callback processed " + std::to_string(processed) + " deferred nodes");
                }
            }
        }
    });
    
    // Register with debug system (can be disabled via config)
    if (_config.enableDebugRegistration) {
        Debug::DebugRegistry::getInstance().registerObject(this, "WorkGraph");
        auto msg = std::format("Created WorkGraph '{}' with contract group", getName());
        ENTROPY_LOG_DEBUG_CAT("WorkGraph", msg);
    }
}

WorkGraph::~WorkGraph() {
    ENTROPY_PROFILE_ZONE();
    
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph destructor starting, pending nodes: " + std::to_string(_pendingNodes.load()));
    }
    
    // Set destroyed flag to prevent new callbacks
    _destroyed.store(true, std::memory_order_release);
    
    // Unregister capacity callback from WorkContractGroup first
    // This prevents new callbacks from being scheduled
    if (_workContractGroup) {
        _workContractGroup->removeOnCapacityAvailable(_capacityCallbackIt);
    }
    
    // Wait for all active callbacks to complete
    if (_activeCallbacks.load(std::memory_order_acquire) > 0) {
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph destructor waiting for callbacks: " + std::to_string(_activeCallbacks.load()));
        }
        std::unique_lock<std::mutex> lock(_waitMutex);
        _shutdownCondition.wait(lock, [this]() {
            return _activeCallbacks.load(std::memory_order_acquire) == 0;
        });
    }
    
    // Now safe to proceed with cleanup
    // Unregister from debug system (if registered)
    if (_config.enableDebugRegistration) {
        Debug::DebugRegistry::getInstance().unregisterObject(this);
        auto msg = std::format("Destroyed WorkGraph '{}'", getName());
        ENTROPY_LOG_DEBUG_CAT("WorkGraph", msg);
    }
}

WorkGraph::NodeHandle WorkGraph::addNode(std::function<void()> work, 
                                        const std::string& name,
                                        void* userData,
                                        ExecutionType executionType) {
    ENTROPY_PROFILE_ZONE();
    std::unique_lock<std::shared_mutex> lock(_graphMutex);
    
    // Create node with the work and execution type
    WorkGraphNode node(std::move(work), name, executionType);
    node.userData = userData;
    
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
    
    // If execution has already started, check if this node can execute immediately
    if (_executionStarted.load(std::memory_order_acquire)) {
        auto* nodeData = handle.getData();
        if (nodeData && nodeData->pendingDependencies.load() == 0) {
            // Try to transition to ready state
            if (_stateManager->transitionState(handle, NodeState::Pending, NodeState::Ready)) {
                // Transition to scheduled before actually scheduling
                if (_stateManager->transitionState(handle, NodeState::Ready, NodeState::Scheduled)) {
                    // Schedule the node
                    _scheduler->scheduleNode(handle);
                }
            }
        }
    }
    
    return handle;
}

WorkGraph::NodeHandle WorkGraph::addYieldableNode(YieldableWorkFunction work,
                                                  const std::string& name,
                                                  void* userData,
                                                  ExecutionType executionType,
                                                  std::optional<uint32_t> maxReschedules) {
    ENTROPY_PROFILE_ZONE();
    std::unique_lock<std::shared_mutex> lock(_graphMutex);
    
    // Create node with yieldable work function
    WorkGraphNode node(std::move(work), name, executionType);
    node.userData = userData;
    node.maxReschedules = maxReschedules;
    
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
    
    if (_config.enableDebugLogging) {
        auto msg = std::format("Added yieldable node '{}' with max reschedules: {}", 
                              name, 
                              maxReschedules.has_value() ? std::to_string(*maxReschedules) : "unlimited");
        ENTROPY_LOG_DEBUG_CAT("WorkGraph", msg);
    }
    
    // If execution has already started, check if this node can execute immediately
    if (_executionStarted.load(std::memory_order_acquire)) {
        auto* nodeData = handle.getData();
        if (nodeData && nodeData->pendingDependencies.load() == 0) {
            // Try to transition to ready state
            if (_stateManager->transitionState(handle, NodeState::Pending, NodeState::Ready)) {
                // Transition to scheduled before actually scheduling
                if (_stateManager->transitionState(handle, NodeState::Ready, NodeState::Scheduled)) {
                    // Schedule the node
                    _scheduler->scheduleNode(handle);
                }
            }
        }
    }
    
    return handle;
}

void WorkGraph::addDependency(NodeHandle from, NodeHandle to) {
    ENTROPY_PROFILE_ZONE();
    std::unique_lock<std::shared_mutex> lock(_graphMutex);
    
    // Add edge in the DAG (this checks for cycles)
    _graph.addEdge(from, to);
    
    // Increment dependency count for the target node
    incrementDependencies(to);
    
    // auto* toData = to.getData();
    // if (toData) {
    //     std::cout << "Added dependency: " << from.getData()->name << " -> " << toData->name 
    //               << " (deps now: " << toData->pendingDependencies.load() << ")" << std::endl;
    // }
}

void WorkGraph::incrementDependencies(NodeHandle node) {
    if (auto* nodeData = node.getData()) {
        auto newCount = nodeData->pendingDependencies.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Node dependencies incremented");
        }
    }
}

size_t WorkGraph::scheduleRoots() {
    std::shared_lock<std::shared_mutex> lock(_graphMutex);
    return scheduleRootsLocked();
}

size_t WorkGraph::scheduleRootsLocked() {
    // Assumes caller already holds a lock on _graphMutex
    size_t rootCount = 0;
    
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Checking " + std::to_string(_nodeHandles.size()) + " nodes for roots");
    }
    
    // Check all cached handles to find roots (nodes ready to execute)
    size_t nodeIndex = 0;
    for (auto& handle : _nodeHandles) {
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Checking node " + std::to_string(nodeIndex++));
        }
        if (isHandleValid(handle)) {
            auto* nodeData = handle.getData();
            
            // Check if this node is ready to execute (no pending dependencies)
            if (nodeData && nodeData->pendingDependencies.load() == 0) {
                if (_config.enableDebugLogging) {
                    ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Found root node with 0 dependencies");
                }
                // Try to transition to ready state through state manager
                if (_stateManager->transitionState(handle, NodeState::Pending, NodeState::Ready)) {
                    // Now transition to scheduled before actually scheduling
                    if (_stateManager->transitionState(handle, NodeState::Ready, NodeState::Scheduled)) {
                        if (_config.enableDebugLogging) {
                            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: About to call scheduler->scheduleNode");
                        }
                        // Schedule the root node
                        bool scheduled = _scheduler->scheduleNode(handle);
                        if (scheduled) {
                            rootCount++;
                            if (_config.enableDebugLogging) {
                                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Root node scheduled or deferred");
                            }
                        } else {
                            if (_config.enableDebugLogging) {
                                ENTROPY_LOG_WARNING_CAT("Concurrency", "WorkGraph: Failed to schedule root node");
                            }
                        }
                    }
                } else if (_config.enableDebugLogging) {
                    ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Failed to transition root node to Ready state");
                }
            }
        }
    }
    
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Root node scheduling complete, scheduled " + std::to_string(rootCount) + " roots");
    }
    
    return rootCount;
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
                ENTROPY_LOG_DEBUG_CAT("WorkGraph", "Processed " + std::to_string(processed) + " deferred nodes after resume");
            }
        }
        
        // Check if any nodes became ready while we were suspended and schedule them
        std::shared_lock<std::shared_mutex> lock(_graphMutex);
        for (const auto& handle : _nodeHandles) {
            auto* nodeData = handle.getData();
            if (nodeData && nodeData->state.load() == NodeState::Ready) {
                // Try to transition to scheduled
                if (_stateManager->transitionState(handle, NodeState::Ready, NodeState::Scheduled)) {
                    _scheduler->scheduleNode(handle);
                }
            }
        }
    }
}

void WorkGraph::execute() {
    ENTROPY_PROFILE_ZONE();
    
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_INFO_CAT("Concurrency", "WorkGraph::execute() starting");
    }
    
    // Need exclusive lock to prevent nodes being added during execution startup
    std::unique_lock<std::shared_mutex> lock(_graphMutex);
    
    bool expected = false;
    if (!_executionStarted.compare_exchange_strong(expected, true)) {
        throw std::runtime_error("WorkGraph execution already started");
    }
    
    // Schedule all root nodes to start execution while holding the lock
    // This eliminates the race window between setting _executionStarted and scheduling roots
    size_t roots = scheduleRootsLocked();
    
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::execute() scheduled " + std::to_string(roots) + " root nodes");
        if (_scheduler) {
            size_t deferred = _scheduler->getDeferredCount();
            if (deferred > 0) {
                ENTROPY_LOG_WARNING_CAT("Concurrency", "WorkGraph::execute() deferred " + std::to_string(deferred) + " nodes during startup");
            }
        }
    }
    
    // Now safe to unlock
    lock.unlock();
    
    // After unlocking, process any deferred nodes
    if (_scheduler) {
        size_t deferred = _scheduler->getDeferredCount();
        if (_config.enableDebugLogging && deferred > 0) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::execute() has " + std::to_string(deferred) + " deferred nodes after initial scheduling");
        }
        if (deferred > 0) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::execute() calling processDeferredNodes");
            }
            size_t processed = _scheduler->processDeferredNodes();
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::execute() processDeferredNodes returned " + std::to_string(processed));
            }
        }
    }
    
    if (roots == 0 && getPendingCount() > 0) {
        auto msg = std::format("ERROR: No roots found. Pending count: {}, node count: {}", 
                  getPendingCount(), _nodeHandles.size());
        ENTROPY_LOG_ERROR_CAT("WorkGraph", msg);
        throw std::runtime_error("WorkGraph has no root nodes but has pending work - possible cycle?");
    }
    
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::execute() completed");
    }
}

bool WorkGraph::scheduleNode(NodeHandle node) {
    // Check if suspended
    if (_suspended.load(std::memory_order_acquire)) {
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::scheduleNode() - graph suspended, deferring node");
        }
        // Don't schedule while suspended
        return false;
    }
    
    if (_config.enableDebugLogging) {
        auto* nodeData = node.getData();
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::scheduleNode() called");
    }
    // Always delegate to the scheduler component
    bool result = _scheduler->scheduleNode(node);
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::scheduleNode() completed");
    }
    return result;
}

void WorkGraph::onNodeComplete(NodeHandle node) {
    auto* nodeData = node.getData();
    if (!nodeData) return;
    
    // Prevent double-processing using atomic flag
    bool expected = false;
    if (!nodeData->completionProcessed.compare_exchange_strong(expected, true, 
                                                                std::memory_order_acq_rel)) {
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_WARNING_CAT("Concurrency", "WorkGraph: Node already processed completion");
        }
        return; // Already processed
    }
    
    // Transition state through state manager
    _stateManager->transitionState(node, NodeState::Executing, NodeState::Completed);
    
    // Update counters
    _completedNodes.fetch_add(1, std::memory_order_relaxed);
    uint32_t pending = _pendingNodes.fetch_sub(1, std::memory_order_acq_rel) - 1;
    
    // If all nodes are complete, notify waiters
    if (pending == 0) {
        std::lock_guard<std::mutex> lock(_waitMutex);
        _waitCondition.notify_all();
    }
    
    // Call completion callback if set
    if (_onNodeComplete) {
        _onNodeComplete(node);
    }
    
    // Schedule children whose dependencies are now satisfied
    // First, copy the children list while holding the lock (minimize critical section)
    std::vector<NodeHandle> children;
    {
        std::shared_lock<std::shared_mutex> lock(_graphMutex);
        children = this->getChildren(node);
    } // Release lock immediately
    
    // Process children outside the lock to minimize contention
    for (auto& child : children) {
        auto* childData = child.getData();
        if (!childData) continue;
        
        // Skip if child is cancelled
        if (childData->state.load(std::memory_order_acquire) == NodeState::Cancelled) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Skipping cancelled child node");
            }
            continue;
        }
        
        // Decrement dependency count
        uint32_t remaining = childData->pendingDependencies.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Child node dependencies decremented");
        }
        
        // If all dependencies satisfied, try to transition to ready and schedule
        if (remaining == 0 && childData->failedParentCount.load(std::memory_order_acquire) == 0) {
            if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Child node is ready - all dependencies satisfied");
            }
            if (_stateManager->transitionState(child, NodeState::Pending, NodeState::Ready)) {
                // Transition to scheduled before actually scheduling
                if (_stateManager->transitionState(child, NodeState::Ready, NodeState::Scheduled)) {
                    // Schedule the child immediately
                    _scheduler->scheduleNode(child);
                }
                if (_config.enableDebugLogging) {
                    ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Scheduled child node");
                }
            } else if (_config.enableDebugLogging) {
                ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Failed to transition child node to Ready state");
            }
        } else if (_config.enableDebugLogging && remaining > 0) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph: Child node still has dependencies");
        }
    }
    
    // Note: Processing of deferred nodes is now handled via the 
    // onCapacityAvailable callback from WorkContractGroup, which
    // is called after contracts are actually freed.
}

WorkGraph::WaitResult WorkGraph::wait() {
    ENTROPY_PROFILE_ZONE();
    
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
        ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::wait() waiting for pending nodes: " + std::to_string(_pendingNodes.load()));
    }
    _waitCondition.wait(lock, [this]() {
        auto pending = _pendingNodes.load(std::memory_order_acquire);
        if (_config.enableDebugLogging && pending > 0) {
            ENTROPY_LOG_DEBUG_CAT("Concurrency", "WorkGraph::wait() still waiting, pending: " + std::to_string(pending));
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
        auto msg = std::format("WorkGraph::wait() - {} nodes were dropped due to deferred queue overflow!", result.droppedCount);
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
                              ", completed: " + std::to_string(completed) +
                              ", failed: " + std::to_string(failed) + 
                              ", dropped: " + std::to_string(dropped));
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

WorkGraph::NodeHandle WorkGraph::addContinuation(const std::vector<NodeHandle>& parents,
                                                std::function<void()> work,
                                                const std::string& name,
                                                ExecutionType executionType) {
    // Create the continuation node with specified execution type
    auto continuation = addNode(std::move(work), name, nullptr, executionType);
    
    // Add dependencies from all parents
    for (const auto& parent : parents) {
        addDependency(parent, continuation);
    }
    
    return continuation;
}

void WorkGraph::onNodeFailed(NodeHandle node) {
    auto* nodeData = node.getData();
    if (!nodeData) return;
    
    // Prevent double-processing
    bool expected = false;
    if (!nodeData->completionProcessed.compare_exchange_strong(expected, true, 
                                                                std::memory_order_acq_rel)) {
        return; // Already processed
    }
    
    // Transition state through state manager
    _stateManager->transitionState(node, NodeState::Executing, NodeState::Failed);
    
    // Update counters
    _failedNodes.fetch_add(1, std::memory_order_relaxed);
    uint32_t pending = _pendingNodes.fetch_sub(1, std::memory_order_acq_rel) - 1;
    
    // If all nodes are complete, notify waiters
    if (pending == 0) {
        std::lock_guard<std::mutex> lock(_waitMutex);
        _waitCondition.notify_all();
    }
    
    // Cancel all dependent nodes
    cancelDependents(node);
    
}

void WorkGraph::onNodeYielded(NodeHandle node) {
    auto* nodeData = node.getData();
    if (!nodeData) return;
    
    // Increment reschedule count
    uint32_t rescheduleCount = nodeData->rescheduleCount.fetch_add(1, std::memory_order_relaxed);
    
    if (_config.enableDebugLogging) {
        auto msg = std::format("Node '{}' yielded (reschedule count: {})", 
                              nodeData->name, rescheduleCount + 1);
        ENTROPY_LOG_DEBUG_CAT("WorkGraph", msg);
    }
    
    // Check reschedule limit
    if (nodeData->maxReschedules && rescheduleCount >= *nodeData->maxReschedules) {
        if (_config.enableDebugLogging) {
            auto msg = std::format("Node '{}' reached max reschedule limit ({}), completing", 
                                  nodeData->name, *nodeData->maxReschedules);
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
    
    // Reschedule the node
    rescheduleYieldedNode(node);
}

void WorkGraph::rescheduleYieldedNode(NodeHandle node) {
    auto* nodeData = node.getData();
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

void WorkGraph::onNodeCancelled(NodeHandle node) {
    auto* nodeData = node.getData();
    if (!nodeData) return;
    
    // Prevent double-processing
    bool expected = false;
    if (!nodeData->completionProcessed.compare_exchange_strong(expected, true, 
                                                                std::memory_order_acq_rel)) {
        return; // Already processed
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

void WorkGraph::cancelDependents(NodeHandle failedNode) {
    std::vector<NodeHandle> nodesToCancel;
    
    {
        std::shared_lock<std::shared_mutex> lock(_graphMutex);
        
        // Get all children of the failed node
        auto children = this->getChildren(failedNode);
        
        for (auto& child : children) {
            auto* childData = child.getData();
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
            auto* nodeData = node.getData();
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

} // namespace Concurrency
} // namespace Core
} // namespace EntropyEngine