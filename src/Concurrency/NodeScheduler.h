/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file NodeScheduler.h
 * @brief Graph node scheduling with overflow management for WorkGraph execution
 * 
 * This file contains the NodeScheduler, which manages the scheduling of graph nodes
 * into WorkContractGroups. It handles overflow scenarios when the work queue is full,
 * maintains execution order, and provides lifecycle callbacks for monitoring.
 */

#pragma once

#include "WorkGraphTypes.h"
#include "WorkContractGroup.h"
#include "../Core/EventBus.h"
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <functional>

namespace EntropyEngine {
namespace Core {
namespace Concurrency {

/**
 * @brief Manages graph node scheduling and overflow handling for work execution
 * 
 * NodeScheduler serves as the interface between WorkGraph's high-level dependency
 * management and WorkContractGroup's low-level execution system. When WorkGraph
 * determines nodes are ready for execution, this scheduler manages their transition
 * into the work queue. The scheduler handles capacity constraints by maintaining a
 * deferred queue for nodes that cannot be immediately scheduled due to queue limitations.
 * 
 * This component bridges the abstraction gap between graph-based task management and
 * queue-based execution. It addresses the practical constraint that ready nodes may
 * exceed available execution slots, providing buffering and advanced scheduling
 * infrastructure.
 * 
 * Key responsibilities:
 * - Immediate scheduling when there's capacity
 * - Deferred queue management when work queue is full
 * - Batch scheduling (schedule multiple nodes in one go)
 * - Lifecycle callbacks for monitoring and debugging
 * - Statistics tracking for analysis
 * 
 * Complexity characteristics:
 * - Deferred queue operations: O(1) push_back, O(1) pop_front
 * 
 * Suitable applications:
 * - Task graphs that might generate bursts of ready nodes
 * - Systems where work generation can outpace execution
 * - Monitoring node execution lifecycle for debugging
 * - Building priority scheduling on top (future enhancement)
 * 
 * Design trade-offs:
 * - Uses mutex for thread safety (not lock-free like WorkContractGroup)
 * - Deque for deferred queue (good cache locality, no allocations until overflow)
 * - Separate stats tracking to avoid polluting hot path
 * 
 * @code
 * // Basic usage with a WorkGraph
 * WorkContractGroup contractGroup(1024);
 * WorkGraph graph;
 * NodeScheduler scheduler(&contractGroup, &graph);
 * 
 * // Set up lifecycle monitoring
 * NodeScheduler::Callbacks callbacks;
 * callbacks.onNodeScheduled = [](NodeHandle node) {
 *     LOG_DEBUG("Node {} scheduled", node.index());
 * };
 * callbacks.onNodeDeferred = [](NodeHandle node) {
 *     LOG_WARN("Node {} deferred (queue full)", node.index());
 * };
 * scheduler.setCallbacks(callbacks);
 * 
 * // Schedule nodes as they become ready
 * if (!scheduler.scheduleNode(readyNode)) {
 *     // Node was deferred, will be scheduled when capacity available
 * }
 * 
 * // Process deferred nodes when work completes
 * size_t scheduled = scheduler.processDeferredNodes();
 * @endcode
 */
class NodeScheduler {
public:
    /**
     * @brief Configuration parameters for tuning scheduler behavior
     * 
     * These settings provide control over memory usage, scheduling overhead, and
     * responsiveness. The defaults work well for most cases, but you might
     * want to tune them based on your workload.
     */
    struct Config {
        size_t maxDeferredNodes;      ///< Maximum nodes to queue when full (prevents unbounded growth)
        bool enableBatchScheduling;   ///< Schedule multiple nodes in one operation (reduces overhead)
        size_t batchSize;             ///< How many nodes to schedule per batch (tune for your workload)
        bool enableDebugLogging;      ///< Enable verbose debug logging for troubleshooting
        
        Config() : maxDeferredNodes(100), enableBatchScheduling(true), batchSize(10), enableDebugLogging(false) {}
    };
    
    /**
     * @brief Lifecycle hooks for monitoring node execution flow
     * 
     * These callbacks let you track what's happening to your nodes as they flow
     * through the scheduling system. Perfect for debugging, profiling, or building
     * visualization tools. All callbacks are optional - only set the ones you need.
     */
    struct Callbacks {
        std::function<void(NodeHandle)> onNodeScheduled;     ///< Node successfully entered work queue
        std::function<void(NodeHandle)> onNodeDeferred;      ///< Node queued due to lack of capacity
        std::function<void(NodeHandle)> onNodeExecuting;     ///< Node started executing (worker picked it up)
        std::function<void(NodeHandle)> onNodeCompleted;     ///< Node finished successfully
        std::function<void(NodeHandle, std::exception_ptr)> onNodeFailed;  ///< Node threw an exception
        std::function<void(NodeHandle)> onNodeDropped;       ///< Node dropped (deferred queue overflow)
        std::function<void(NodeHandle)> onNodeYielded;       ///< Node yielded execution (will reschedule)
    };
    
    /**
     * @brief Creates a scheduler bridging graph nodes to work execution
     * 
     * Sets up scheduling for a WorkGraph. Doesn't own graph or contract group.
     * 
     * @param contractGroup Where to schedule work (must outlive scheduler)
     * @param graph The graph whose nodes we're scheduling (must outlive scheduler) 
     * @param eventBus Optional event system (can be nullptr)
     * @param config Tuning parameters
     * 
     * @code
     * // Typical setup in a WorkGraph
     * auto contractGroup = std::make_unique<WorkContractGroup>(1024);
     * auto scheduler = std::make_unique<NodeScheduler>(
     *     contractGroup.get(), 
     *     this,  // WorkGraph passes itself
     *     _eventBus,
     *     NodeScheduler::Config{.maxDeferredNodes = 200}
     * );
     * @endcode
     */
    NodeScheduler(WorkContractGroup* contractGroup,
                  const WorkGraph* graph,
                  Core::EventBus* eventBus = nullptr,
                  const Config& config = {})
        : _contractGroup(contractGroup)
        , _graph(graph)
        , _eventBus(eventBus)
        , _config(config) {
    }
    
    ~NodeScheduler() {
        // Signal that this scheduler is being destroyed
        _destroyed.store(true, std::memory_order_release);
    }
    
    /**
     * @brief Installs lifecycle monitoring callbacks
     * 
     * Set before scheduling for lifecycle tracking. Not thread-safe with active
     * scheduling - set during init.
     * 
     * @param callbacks Structure with optional callback functions
     * 
     * @code
     * NodeScheduler::Callbacks callbacks;
     * callbacks.onNodeDeferred = [&deferredCount](NodeHandle) { 
     *     deferredCount++; 
     * };
     * callbacks.onNodeDropped = [](NodeHandle node) {
     *     LOG_ERROR("Critical: Node {} dropped!", node.index());
     * };
     * scheduler.setCallbacks(callbacks);
     * @endcode
     */
    void setCallbacks(const Callbacks& callbacks) {
        _callbacks = callbacks;
    }
    
    /**
     * @brief Attempts to schedule a node, deferring if necessary
     * 
     * Main entry point for execution. Tries immediate scheduling, defers if full,
     * drops if deferred queue full. Thread-safe.
     * 
     * @param node Handle to the node that's ready to execute
     * @return true if scheduled immediately, false if deferred or dropped
     * 
     * @code
     * // In your graph's "node became ready" logic
     * if (scheduler.scheduleNode(readyNode)) {
     *     // Great, it's in the work queue
     * } else {
     *     // Deferred or dropped - check stats to see which
     *     auto stats = scheduler.getStats();
     *     if (stats.nodesDropped > 0) {
     *         // Houston, we have a problem
     *     }
     * }
     * @endcode
     */
    bool scheduleNode(NodeHandle node);
    
    /**
     * @brief Explicitly defers a node without trying to schedule first
     * 
     * Bypasses capacity check, goes straight to deferred queue. Useful for
     * batch operations when you know there's no capacity.
     * 
     * @param node The node to add to deferred queue
     * @return true if queued successfully, false if deferred queue is full
     * 
     * @code
     * // When you know the work queue is full
     * if (!scheduler.hasCapacity()) {
     *     // Don't even try to schedule, just defer
     *     for (auto& node : readyNodes) {
     *         if (!scheduler.deferNode(node)) {
     *             LOG_ERROR("Deferred queue full!");
     *             break;
     *         }
     *     }
     * }
     * @endcode
     */
    bool deferNode(NodeHandle node);
    
    /**
     * @brief Drains the deferred queue into available execution slots
     * 
     * Pulls nodes from deferred queue (FIFO) until empty or out of capacity.
     * Use maxToSchedule to leave room for new work.
     * 
     * @param maxToSchedule How many to schedule max (0 = all possible)
     * @return Number of nodes actually scheduled
     * 
     * @code
     * // After work completes, process waiting nodes
     * void onWorkCompleted() {
     *     size_t scheduled = scheduler.processDeferredNodes();
     *     if (scheduled > 0) {
     *         LOG_DEBUG("Scheduled {} deferred nodes", scheduled);
     *     }
     * }
     * 
     * // Or limit to leave room for new work
     * size_t availableSlots = scheduler.getAvailableCapacity();
     * scheduler.processDeferredNodes(availableSlots / 2);  // Use half for deferred
     * @endcode
     */
    size_t processDeferredNodes(size_t maxToSchedule = 0);
    
    /**
     * @brief Quick check if we can accept more work right now
     * 
     * Snapshot that may be stale. Use for optimization hints, not critical logic.
     * 
     * @return true if work queue has free slots, false if full
     * 
     * @code
     * // Use for early-out optimization
     * if (!scheduler.hasCapacity()) {
     *     // Don't bother trying to schedule a bunch of nodes
     *     return;  
     * }
     * @endcode
     */
    bool hasCapacity() const {
        return _contractGroup->activeCount() < _contractGroup->capacity();
    }
    
    /**
     * @brief Gets exact number of free execution slots
     * 
     * More precise than hasCapacity(). Snapshot that might be stale.
     * 
     * @return Number of nodes that could be scheduled immediately
     * 
     * @code
     * // Batch scheduling optimization
     * size_t capacity = scheduler.getAvailableCapacity();
     * size_t toSchedule = std::min(capacity, readyNodes.size());
     * for (size_t i = 0; i < toSchedule; ++i) {
     *     scheduler.scheduleNode(readyNodes[i]);
     * }
     * @endcode
     */
    size_t getAvailableCapacity() const {
        size_t active = _contractGroup->activeCount();
        size_t capacity = _contractGroup->capacity();
        return (active < capacity) ? (capacity - active) : 0;
    }
    
    /**
     * @brief Checks how many nodes are waiting in the deferred queue
     * 
     * Monitor system pressure. Thread-safe but takes lock - avoid tight loops.
     * 
     * @return Current number of nodes waiting for execution capacity
     * 
     * @code
     * // Monitor for queue buildup
     * size_t deferred = scheduler.getDeferredCount();
     * if (deferred > 50) {
     *     LOG_WARN("High scheduling pressure: {} nodes waiting", deferred);
     * }
     * @endcode
     */
    size_t getDeferredCount() const {
        std::shared_lock<std::shared_mutex> lock(_deferredMutex);
        return _deferredQueue.size();
    }
    
    /**
     * @brief Nuclear option: drops all deferred nodes
     * 
     * For aborting pending work. Nodes are lost - no execution or callbacks.
     * 
     * @return How many nodes were dropped from the queue
     * 
     * @code
     * // Emergency abort
     * size_t dropped = scheduler.clearDeferredNodes();
     * if (dropped > 0) {
     *     LOG_WARN("Dropped {} pending nodes during abort", dropped);
     * }
     * @endcode
     */
    size_t clearDeferredNodes() {
        std::lock_guard<std::shared_mutex> lock(_deferredMutex);  // Exclusive lock for writing
        size_t count = _deferredQueue.size();
        _deferredQueue.clear();
        return count;
    }
    
    /**
     * @brief Batch scheduling for multiple ready nodes
     * 
     * Handles multiple nodes efficiently. Schedules what fits, defers rest.
     * Preserves order.
     * 
     * @param nodes Vector of nodes ready for execution
     * @return Number scheduled immediately (rest deferred/dropped)
     * 
     * @code
     * // After a node completes, schedule all its dependents
     * std::vector<NodeHandle> readyDependents = getDependents(completed);
     * size_t scheduled = scheduler.scheduleReadyNodes(readyDependents);
     * LOG_DEBUG("Scheduled {}/{} dependent nodes", 
     *           scheduled, readyDependents.size());
     * @endcode
     */
    size_t scheduleReadyNodes(const std::vector<NodeHandle>& nodes);
    
    /**
     * @brief Health metrics for the scheduler
     * 
     * These stats help you understand scheduling behavior and identify bottlenecks.
     * If nodesDropped > 0, you're losing work and need to increase maxDeferredNodes.
     * If peakDeferred is high, you might need more workers or to adjust node execution.
     */
    struct Stats {
        size_t nodesScheduled = 0;    ///< Total nodes that went straight to execution
        size_t nodesDeferred = 0;     ///< Total nodes that had to wait in deferred queue
        size_t nodesDropped = 0;      ///< Critical: nodes lost due to queue overflow!
        size_t currentDeferred = 0;   ///< Current size of deferred queue
        size_t peakDeferred = 0;      ///< Highest deferred queue size seen
    };
    
    Stats getStats() const {
        std::lock_guard<std::mutex> lock(_statsMutex);
        Stats stats = _stats;
        stats.currentDeferred = getDeferredCount();
        return stats;
    }
    
    /**
     * @brief Clears all statistics counters back to zero
     * 
     * For benchmarking or resetting after warmup. Only counters, not queue.
     * 
     * @code
     * scheduler.resetStats();
     * runBenchmark();
     * auto stats = scheduler.getStats();
     * LOG_INFO("Benchmark: {} scheduled, {} deferred", 
     *          stats.nodesScheduled, stats.nodesDeferred);
     * @endcode
     */
    void resetStats() {
        std::lock_guard<std::mutex> lock(_statsMutex);
        _stats = Stats{};
    }
    
    /**
     * @brief Estimates memory consumption of the scheduler
     * 
     * Includes object + deferred queue. Conservative estimate.
     * 
     * @return Approximate bytes used by this scheduler instance
     * 
     * @code
     * // Check memory pressure
     * if (scheduler.getMemoryUsage() > 1024 * 1024) {  // 1MB
     *     LOG_WARN("Scheduler using {}KB of memory", 
     *              scheduler.getMemoryUsage() / 1024);
     * }
     * @endcode
     */
    size_t getMemoryUsage() const {
        std::shared_lock<std::shared_mutex> lock(_deferredMutex);  // Shared lock for reading
        return sizeof(*this) + _deferredQueue.size() * sizeof(NodeHandle);
    }
    
private:
    WorkContractGroup* _contractGroup;         ///< Where we schedule work (not owned)
    const WorkGraph* _graph;                   ///< Graph we're scheduling for (not owned)
    Core::EventBus* _eventBus;                 ///< Optional event system for notifications
    Config _config;                            ///< Scheduler configuration
    Callbacks _callbacks;                      ///< Lifecycle event callbacks
    
    // Safety flag to prevent use after destruction
    mutable std::atomic<bool> _destroyed{false};  ///< Set to true in destructor for safety checks
    
    // Deferred queue for nodes waiting for capacity
    mutable std::shared_mutex _deferredMutex;   ///< Reader-writer lock for deferred queue (mutable for const methods)
    std::deque<NodeHandle> _deferredQueue;      ///< FIFO queue of nodes waiting for capacity
    
    // Statistics
    mutable std::mutex _statsMutex;           ///< Protects statistics (separate to reduce contention)
    Stats _stats;                             ///< Accumulated statistics
    
    /**
     * @brief Creates the lambda that will be executed by workers
     * 
     * Wraps node work with lifecycle callbacks and error handling. Handles
     * onNodeExecuting, work execution, onNodeCompleted/Failed.
     * Private - internal bridge between graph and execution.
     * 
     * @param node The node whose work we're wrapping
     * @return Lambda function suitable for WorkContractGroup execution
     */
    std::function<void()> createWorkWrapper(NodeHandle node);
    
    /**
     * @brief Thread-safe statistics update helper
     * 
     * Updates counters and tracks peak deferred size. Private - auto-updated.
     * 
     * @param scheduled True if node was scheduled immediately
     * @param deferred True if node was added to deferred queue
     * @param dropped True if node was dropped (queue overflow)
     */
    void updateStats(bool scheduled, bool deferred, bool dropped);
    
    /**
     * @brief Publishes a "node scheduled" event to the event bus
     * 
     * Only if event bus configured. Private - internal side effect.
     * 
     * @param node The node that was scheduled
     */
    void publishScheduledEvent(NodeHandle node);
    
    /**
     * @brief Publishes a "node deferred" event to the event bus
     * 
     * @param node The node that was deferred
     */
    void publishDeferredEvent(NodeHandle node);
};

} // namespace Concurrency
} // namespace Core
} // namespace EntropyEngine