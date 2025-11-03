/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "NodeScheduler.h"
#include "WorkGraphEvents.h"
#include "WorkGraph.h"
#include "../Logging/Logger.h"
#include <algorithm>
#include <format>

#ifdef EntropyDarwin
using std::min;
using std::max;
#endif

namespace EntropyEngine {
namespace Core {
namespace Concurrency {

bool NodeScheduler::scheduleNode(NodeHandle node) {
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("NodeScheduler", "scheduleNode() called");
    }
    auto* dag = node.handleOwnerAs<Graph::DirectedAcyclicGraph<WorkGraphNode>>();
    auto* nodeData = dag ? dag->getNodeData(node) : nullptr;
    if (!nodeData) {
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("NodeScheduler", "scheduleNode() - no node data");
        }
        return false;
    }
    
    // Check capacity
    if (!hasCapacity()) {
        // Try to defer instead
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("NodeScheduler", "No capacity, deferring node");
        }
        bool deferred = deferNode(node);
        if (_config.enableDebugLogging) {
            ENTROPY_LOG_DEBUG_CAT("NodeScheduler", "deferNode() returned " + std::to_string(deferred));
        }
        return deferred;
    }
    
    // Create work wrapper
    auto work = createWorkWrapper(node);
    
    // Create contract with the node's execution type
    auto handle = _contractGroup->createContract(std::move(work), nodeData->executionType);
    if (!handle.valid()) {
        // Contract group refused - try to defer
        return deferNode(node);
    }
    
    // Store handle in node
    nodeData->handle = handle;
    
    // Schedule the contract
    auto result = handle.schedule();
    if (result != ScheduleResult::Scheduled) {
        // Failed to schedule - defer it
        nodeData->handle = WorkContractHandle();  // Clear invalid handle
        return deferNode(node);
    }
    
    // Update statistics
    updateStats(true, false, false);
    
    // Publish event
    publishScheduledEvent(node);
    
    // Notify callback
    if (_callbacks.onNodeScheduled) {
        _callbacks.onNodeScheduled(node);
    }
    
    return true;
}

bool NodeScheduler::deferNode(NodeHandle node) {
    std::lock_guard<std::shared_mutex> lock(_deferredMutex);  // Exclusive lock for modifying queue
    
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("NodeScheduler", "Deferring node, queue size: " + std::to_string(_deferredQueue.size()) + ", max: " + std::to_string(_config.maxDeferredNodes));
    }
    
    // Check queue capacity (0 = unlimited)
    if (_config.maxDeferredNodes > 0 && _deferredQueue.size() >= _config.maxDeferredNodes) {
        // Queue full - drop the node
        auto msg = std::format("NodeScheduler dropping node - deferred queue full (max: {})", 
                               _config.maxDeferredNodes);
        ENTROPY_LOG_ERROR_CAT("NodeScheduler", msg);
        updateStats(false, false, true);
        
        // Notify callback about dropped node
        if (_callbacks.onNodeDropped) {
            _callbacks.onNodeDropped(node);
        }
        
        return false;
    }
    
    // Add to deferred queue
    _deferredQueue.push_back(node);
    
    // Update statistics
    updateStats(false, true, false);
    
    // Track peak deferred count
    {
        std::lock_guard<std::mutex> statsLock(_statsMutex);
        _stats.peakDeferred = std::max(_stats.peakDeferred, _deferredQueue.size());
    }
    
    // Publish event
    publishDeferredEvent(node);
    
    // Notify callback
    if (_callbacks.onNodeDeferred) {
        _callbacks.onNodeDeferred(node);
    }
    
    if (_config.enableDebugLogging) {
        ENTROPY_LOG_DEBUG_CAT("NodeScheduler", "Node deferred successfully, queue size now: " + std::to_string(_deferredQueue.size()));
    }
    
    return true;
}

size_t NodeScheduler::processDeferredNodes(size_t maxToSchedule) {
    // Determine how many to process
    size_t toProcess = maxToSchedule;
    if (toProcess == 0) {
        toProcess = getAvailableCapacity();
    }
    
    if (toProcess == 0) {
        return 0;  // No capacity
    }
    
    // Extract nodes from deferred queue
    std::vector<NodeHandle> nodesToSchedule;
    {
        std::lock_guard<std::shared_mutex> lock(_deferredMutex);  // Exclusive lock for modifying queue
        
        size_t count = std::min(toProcess, _deferredQueue.size());
        nodesToSchedule.reserve(count);
        
        for (size_t i = 0; i < count; ++i) {
            nodesToSchedule.push_back(_deferredQueue.front());
            _deferredQueue.pop_front();
        }
    }
    
    // Schedule the nodes
    size_t scheduled = 0;
    for (const auto& node : nodesToSchedule) {
        if (scheduleNode(node)) {
            scheduled++;
        } else {
            // Scheduling failed - node was re-deferred or dropped
            break;  // Stop if we hit capacity
        }
    }
    
    return scheduled;
}

size_t NodeScheduler::scheduleReadyNodes(const std::vector<NodeHandle>& nodes) {
    size_t scheduled = 0;
    
    // Try batch scheduling if enabled
    if (_config.enableBatchScheduling && nodes.size() > 1) {
        // Schedule in batches for better efficiency
        for (size_t i = 0; i < nodes.size(); i += _config.batchSize) {
            size_t batchEnd = std::min(i + _config.batchSize, nodes.size());
            
            for (size_t j = i; j < batchEnd; ++j) {
                if (scheduleNode(nodes[j])) {
                    scheduled++;
                } else {
                    // Hit capacity - stop scheduling
                    return scheduled;
                }
            }
            
            // Check if we should continue
            if (!hasCapacity()) {
                break;
            }
        }
    } else {
        // Schedule one by one
        for (const auto& node : nodes) {
            if (scheduleNode(node)) {
                scheduled++;
            } else {
                // Hit capacity - stop scheduling
                break;
            }
        }
    }
    
    return scheduled;
}

std::function<void()> NodeScheduler::createWorkWrapper(NodeHandle node) {
    return [this, node]() {
        // Check if scheduler has been destroyed
        if (_destroyed.load(std::memory_order_acquire)) {
            return;  // Scheduler is gone, do nothing
        }
        
        auto* dag = node.handleOwnerAs<Graph::DirectedAcyclicGraph<WorkGraphNode>>();
        auto* nodeData = dag ? dag->getNodeData(node) : nullptr;
        if (!nodeData) {
            return;
        }
        
        // Notify execution starting (check destroyed flag before each callback)
        if (!_destroyed.load(std::memory_order_acquire) && _callbacks.onNodeExecuting) {
            _callbacks.onNodeExecuting(node);
        }
        
        // Publish event
        if (!_destroyed.load(std::memory_order_acquire) && _eventBus) {
            _eventBus->publish(NodeExecutingEvent(_graph, node));
        }
        
        // Execute the work based on variant type
        bool failed = false;
        bool yielded = false;
        bool yieldedUntil = false;
        std::chrono::steady_clock::time_point wakeTime;
        std::exception_ptr exception;

        try {
            if (nodeData->isYieldable) {
                // Handle yieldable work function
                if (auto* yieldableWork = std::get_if<YieldableWorkFunction>(&nodeData->work)) {
                    WorkResultContext result = (*yieldableWork)();
                    if (result.result == WorkResult::Yield) {
                        yielded = true;
                    } else if (result.result == WorkResult::YieldUntil && result.wakeTime) {
                        // Validate that wake time is in the future or current
                        auto now = std::chrono::steady_clock::now();
                        if (*result.wakeTime >= now) {
                            // Future or current time - defer until then
                            yieldedUntil = true;
                            wakeTime = *result.wakeTime;
                        } else {
                            // Past time - treat as immediate yield
                            yielded = true;
                        }
                    }
                }
            } else {
                // Handle legacy void work function
                if (auto* voidWork = std::get_if<std::function<void()>>(&nodeData->work)) {
                    (*voidWork)();
                }
            }
        } catch (...) {
            failed = true;
            exception = std::current_exception();
        }

        // Check if destroyed before notifying completion
        if (_destroyed.load(std::memory_order_acquire)) {
            return;  // Scheduler is gone, skip cleanup
        }

        // Notify completion, failure, or yield
        if (failed) {
            if (_callbacks.onNodeFailed) {
                _callbacks.onNodeFailed(node, exception);
            }
        } else if (yieldedUntil) {
            // Notify timed yield via callback
            if (_callbacks.onNodeYieldedUntil) {
                _callbacks.onNodeYieldedUntil(node, wakeTime);
            }
        } else if (yielded) {
            // Notify immediate yield via callback
            if (_callbacks.onNodeYielded) {
                _callbacks.onNodeYielded(node);
            }
        } else {
            if (_callbacks.onNodeCompleted) {
                _callbacks.onNodeCompleted(node);
            }
        }
        
        // Note: We cannot process deferred nodes here because the contract
        // hasn't been freed yet. The WorkService will call completeExecution()
        // AFTER this wrapper returns, and only then will capacity be available.
    };
}

void NodeScheduler::updateStats(bool scheduled, bool deferred, bool dropped) {
    std::lock_guard<std::mutex> lock(_statsMutex);
    
    if (scheduled) {
        _stats.nodesScheduled++;
    }
    if (deferred) {
        _stats.nodesDeferred++;
    }
    if (dropped) {
        _stats.nodesDropped++;
    }
}

void NodeScheduler::publishScheduledEvent(NodeHandle node) {
    if (_eventBus) {
        _eventBus->publish(NodeScheduledEvent(_graph, node));
    }
}

void NodeScheduler::publishDeferredEvent(NodeHandle node) {
    if (_eventBus) {
        // Note: This is called from deferNode() which already holds an exclusive lock
        // We can safely read the queue size here without additional locking
        size_t queueSize = _deferredQueue.size();
        _eventBus->publish(NodeDeferredEvent(_graph, node, queueSize));
    }
}

bool NodeScheduler::deferNodeUntil(NodeHandle node, std::chrono::steady_clock::time_point wakeTime) {
    std::lock_guard<std::shared_mutex> lock(_timedDeferredMutex);

    if (_config.enableDebugLogging) {
        auto now = std::chrono::steady_clock::now();
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(wakeTime - now);
        ENTROPY_LOG_DEBUG_CAT("NodeScheduler",
            "Deferring node until wake time (delay: " + std::to_string(delay.count()) + "ms)");
    }

    // Add to timed deferred queue (priority queue sorted by wake time)
    _timedDeferredQueue.push({node, wakeTime});

    return true;
}

size_t NodeScheduler::processTimedDeferredNodes(size_t maxToSchedule) {
    auto now = std::chrono::steady_clock::now();

    // Extract nodes whose wake time has passed (preserve full TimedNode for wake time tracking)
    std::vector<TimedNode> readyNodes;
    {
        std::lock_guard<std::shared_mutex> lock(_timedDeferredMutex);

        // Pop all nodes that are ready (wake time <= now)
        while (!_timedDeferredQueue.empty() &&
               _timedDeferredQueue.top().wakeTime <= now) {
            readyNodes.push_back(_timedDeferredQueue.top());
            _timedDeferredQueue.pop();

            // Check if we've hit the limit (0 means no limit)
            if (maxToSchedule > 0 && readyNodes.size() >= maxToSchedule) {
                break;
            }
        }
    }

    if (_config.enableDebugLogging && !readyNodes.empty()) {
        ENTROPY_LOG_DEBUG_CAT("NodeScheduler",
            "Processing " + std::to_string(readyNodes.size()) + " timed deferred nodes");
    }

    // Schedule the ready nodes
    size_t scheduled = 0;
    for (size_t i = 0; i < readyNodes.size(); ++i) {
        if (scheduleNode(readyNodes[i].node)) {
            scheduled++;
        } else {
            // Scheduling failed - hit capacity
            // Re-defer remaining nodes back to timed queue with original wake times
            std::lock_guard<std::shared_mutex> lock(_timedDeferredMutex);
            for (size_t j = i; j < readyNodes.size(); ++j) {
                // Re-enqueue with preserved original wake time for scheduling precision
                _timedDeferredQueue.push(readyNodes[j]);
            }
            break;
        }
    }

    return scheduled;
}

} // namespace Concurrency
} // namespace Core
} // namespace EntropyEngine