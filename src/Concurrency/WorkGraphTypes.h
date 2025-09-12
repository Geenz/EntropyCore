/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file WorkGraphTypes.h
 * @brief Common types and enums for the WorkGraph system - the vocabulary of task orchestration
 * 
 * This file contains all the type definitions, enums, and configuration structures used
 * throughout the WorkGraph system. It's kept separate to avoid circular dependencies
 * and to provide a clean, central place for all the types you'll work with.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <chrono>
#include "../Graph/AcyclicNodeHandle.h"

namespace EntropyEngine {
namespace Core {
    // Forward declaration
    class EventBus;
    
namespace Concurrency {
    
    // Forward declarations
    struct WorkGraphNode;
    class WorkGraph;
    class WorkContractGroup;
    class NodeScheduler;
    class NodeStateManager;

    /**
     * @brief Defines where a work contract can be executed
     *
     * Choose AnyThread for CPU-bound work that can run anywhere. Choose MainThread
     * for UI updates, OpenGL calls, or other operations that must run on a specific
     * thread. Dependencies work seamlessly across execution types.
     * 
     * @code
     * // Background processing
     * auto compute = graph.addNode([]{ heavyComputation(); }, 
     *                              "compute", nullptr, ExecutionType::AnyThread);
     * 
     * // UI update that depends on computation
     * auto update = graph.addNode([]{ updateProgressBar(); },
     *                             "update-ui", nullptr, ExecutionType::MainThread);
     * 
     * graph.addDependency(compute, update);  // UI waits for computation
     * @endcode
     */
    enum class ExecutionType : uint8_t {
        AnyThread = 0,  ///< Runs on any worker thread from the pool
        MainThread = 1  ///< Must run on the main/UI thread
    };
    
    /**
     * @brief The lifecycle states of a task node - from birth to completion
     * 
     * Every node in your WorkGraph moves through these states as it progresses from
     * "waiting for parents" to "done". The transitions are carefully controlled to
     * ensure thread safety and proper dependency management.
     * 
     * State flow:
     * - Pending → Ready (when all dependencies complete)
     * - Ready → Scheduled (when submitted to thread pool)
     * - Scheduled → Executing (when thread picks it up)
     * - Executing → Completed/Failed/Yielded (based on return value or exceptions)
     * - Yielded → Ready (for rescheduling)
     * - Any state → Cancelled (if parent fails)
     * 
     * Terminal states (Completed, Failed, Cancelled) are final - no further transitions.
     */
    enum class NodeState : uint8_t {
        Pending   = 0,  ///< Waiting for dependencies - can't run yet
        Ready     = 1,  ///< All dependencies satisfied, waiting for thread
        Scheduled = 2,  ///< Submitted to WorkContractGroup, in queue
        Executing = 3,  ///< Currently running on a worker thread
        Completed = 4,  ///< Finished successfully - triggered children
        Failed    = 5,  ///< Exception thrown - children will be cancelled
        Cancelled = 6,  ///< Skipped due to parent failure - never ran
        Yielded   = 7   ///< Suspended execution, will be rescheduled
    };
    
    /**
     * @brief Return value from yieldable work functions
     * 
     * Work functions can now return a status to control their execution flow.
     * Complete means the work is done, Yield means suspend and reschedule later.
     * This enables coroutine-like behavior without actual C++ coroutines.
     * 
     * @code
     * auto node = graph.addYieldableNode([]() -> WorkResult {
     *     if (!dataReady()) {
     *         return WorkResult::Yield;  // Try again later
     *     }
     *     processData();
     *     return WorkResult::Complete;
     * });
     * @endcode
     */
    enum class WorkResult : uint8_t {
        Complete = 0,   ///< Work is done, proceed to completion
        Yield = 1       ///< Suspend and reschedule for later execution
    };
    
    /**
     * @brief The final verdict on how a node's execution went
     * 
     * Simple enum to categorize the outcome of node execution. Used in callbacks
     * and events to communicate results without needing to check multiple states.
     */
    enum class ExecutionResult : uint8_t {
        Success = 0,    ///< Work function completed without throwing
        Failed = 1,     ///< Work function threw an exception
        Cancelled = 2,  ///< Never ran due to parent failure
        Skipped = 3     ///< Skipped for other reasons (reserved for future use)
    };
    
    /**
     * @brief Configuration parameters and feature toggles for WorkGraph instances
     * 
     * WorkGraph supports various complexity levels from minimal to feature-rich configurations.
     * This structure provides control over optional features and tuning parameters.
     * Default values provide a lightweight graph configuration.
     * 
     * The configuration allows customization of graph behavior through feature enablement,
     * tuning, and resource management options to match specific use case requirements.
     * 
     * @code
     * // Minimal config for simplified setup
     * WorkGraphConfig fastConfig;
     * fastConfig.expectedNodeCount = 1000;  // Pre-allocate storage
     * 
     * // Rich config for debugging and monitoring
     * WorkGraphConfig debugConfig;
     * debugConfig.enableEvents = true;
     * debugConfig.enableDebugLogging = true;
     * debugConfig.enableDebugRegistration = true;
     * 
     * // Custom memory management
     * WorkGraphConfig customConfig;
     * customConfig.nodeAllocator = myPoolAllocator;
     * customConfig.nodeDeallocator = myPoolDeallocator;
     * @endcode
     */
    struct WorkGraphConfig {
        /// Enable event bus for this graph - enables monitoring
        bool enableEvents = false;
        
        /// Enable advanced state management - for complex workflows with state machines
        bool enableStateManager = false;
        
        /// Enable advanced scheduling features - priority queues, affinity, etc.
        bool enableAdvancedScheduling = false;
        
        /// Expected number of nodes (for pre-allocation) - avoids reallocation during execution
        size_t expectedNodeCount = 16;
        
        /// Maximum deferred queue size (0 = unlimited) - prevents unbounded memory growth
        size_t maxDeferredNodes = 0;  // Unlimited by default
        
        /// Maximum iterations when processing deferred nodes - controls how aggressively we fill capacity
        /// Higher values ensure more aggressive filling of available capacity
        size_t maxDeferredProcessingIterations = 10;
        
        /// Enable debug logging - verbose output for troubleshooting
        bool enableDebugLogging = false;
        
        /// Enable debug registration - makes graph visible in debug tools
        bool enableDebugRegistration = false;
        
        /// Use shared event bus instead of creating own - for system-wide event correlation
        std::shared_ptr<Core::EventBus> sharedEventBus = nullptr;
        
        /// Custom allocator for node storage - integrate with your memory system
        std::function<void*(size_t)> nodeAllocator = nullptr;
        std::function<void(void*)> nodeDeallocator = nullptr;
    };
    
    /**
     * @brief Real-time metrics for your executing workflow - watch the action unfold
     * 
     * These statistics are updated atomically as your graph executes, giving you
     * a live view of what's happening. Since multiple threads update these counters
     * simultaneously, they might be slightly inconsistent if read individually.
     * 
     * That's why we provide toSnapshot() - it grabs all values at once for a
     * coherent view. Perfect for progress bars, dashboards, or post-mortem analysis.
     * 
     * Memory usage tracking helps you understand the footprint of large workflows.
     * Execution time is wall-clock time from first node start to last node finish.
     */
    struct WorkGraphStats {
        std::atomic<uint32_t> totalNodes{0};         ///< How many nodes exist in the graph
        std::atomic<uint32_t> completedNodes{0};     ///< Successfully finished nodes
        std::atomic<uint32_t> failedNodes{0};        ///< Nodes that threw exceptions
        std::atomic<uint32_t> cancelledNodes{0};     ///< Nodes skipped due to parent failure
        std::atomic<uint32_t> pendingNodes{0};       ///< Waiting for dependencies
        std::atomic<uint32_t> readyNodes{0};         ///< Ready but not yet scheduled
        std::atomic<uint32_t> scheduledNodes{0};     ///< In the work queue
        std::atomic<uint32_t> executingNodes{0};     ///< Currently running
        
        std::atomic<size_t> memoryUsage{0};          ///< Approximate memory consumption
        std::chrono::steady_clock::duration totalExecutionTime{};  ///< Total wall time
        
        /**
         * @brief Frozen moment in time - all stats captured atomically
         * 
         * Since the live stats change constantly, this snapshot gives you a
         * consistent view where all numbers add up correctly. The individual
         * atomic loads use relaxed ordering for speed - we don't need strict
         * ordering since we're just reading counters.
         */
        struct Snapshot {
            uint32_t totalNodes = 0;
            uint32_t completedNodes = 0;
            uint32_t failedNodes = 0;
            uint32_t cancelledNodes = 0;
            uint32_t pendingNodes = 0;
            uint32_t readyNodes = 0;
            uint32_t scheduledNodes = 0;
            uint32_t executingNodes = 0;
            size_t memoryUsage = 0;
            std::chrono::steady_clock::duration totalExecutionTime{};
        };
        
        /**
         * @brief Capture all statistics in one consistent snapshot
         * @return Snapshot with all counters captured atomically
         */
        Snapshot toSnapshot() const {
            Snapshot snap;
            snap.totalNodes = totalNodes.load(std::memory_order_relaxed);
            snap.completedNodes = completedNodes.load(std::memory_order_relaxed);
            snap.failedNodes = failedNodes.load(std::memory_order_relaxed);
            snap.cancelledNodes = cancelledNodes.load(std::memory_order_relaxed);
            snap.pendingNodes = pendingNodes.load(std::memory_order_relaxed);
            snap.readyNodes = readyNodes.load(std::memory_order_relaxed);
            snap.scheduledNodes = scheduledNodes.load(std::memory_order_relaxed);
            snap.executingNodes = executingNodes.load(std::memory_order_relaxed);
            snap.memoryUsage = memoryUsage.load(std::memory_order_relaxed);
            snap.totalExecutionTime = totalExecutionTime;
            return snap;
        }
    };
    
    // Type aliases for clarity - the common types you'll use everywhere
    using NodeHandle = Graph::AcyclicNodeHandle<WorkGraphNode>;          ///< How you reference nodes
    using NodeCallback = std::function<void(NodeHandle)>;                ///< Callbacks that receive nodes
    using WorkFunction = std::function<void()>;                          ///< The actual work to execute (legacy)
    using YieldableWorkFunction = std::function<WorkResult()>;           ///< Work that can yield/suspend
    using CompletionCallback = std::function<void(ExecutionResult)>;     ///< Notified when work completes
    
    /**
     * @brief Check if a node has reached the end of its journey
     * 
     * Terminal states (Completed, Failed, Cancelled) are final.
     * Yielded is NOT terminal - the node will resume execution.
     * 
     * @param state The state to check
     * @return true if this is a final state
     */
    inline constexpr bool isTerminalState(NodeState state) {
        return state == NodeState::Completed || 
               state == NodeState::Failed || 
               state == NodeState::Cancelled;
    }
    
    /**
     * @brief Validates state transitions - prevents impossible state changes
     * 
     * Valid transitions:
     * - Pending → Ready or Cancelled
     * - Ready → Scheduled or Cancelled
     * - Scheduled → Executing or Cancelled
     * - Executing → Completed, Failed, or Yielded
     * - Yielded → Ready (for rescheduling)
     * - Terminal states → Nothing
     * 
     * @param from Current state
     * @param to Desired new state
     * @return true if the transition is legal
     */
    inline constexpr bool isValidTransition(NodeState from, NodeState to) {
        // Terminal states cannot transition
        if (isTerminalState(from)) {
            return false;
        }
        
        // Define valid transitions
        switch (from) {
            case NodeState::Pending:
                return to == NodeState::Ready || to == NodeState::Cancelled;
                
            case NodeState::Ready:
                return to == NodeState::Scheduled || to == NodeState::Cancelled;
                
            case NodeState::Scheduled:
                return to == NodeState::Executing || to == NodeState::Cancelled;
                
            case NodeState::Executing:
                return to == NodeState::Completed || to == NodeState::Failed || to == NodeState::Yielded;
                
            case NodeState::Yielded:
                return to == NodeState::Ready || to == NodeState::Cancelled;
                
            default:
                return false;
        }
    }
    
    /**
     * @brief Human-readable state names for logging and debugging
     * 
     * @param state The state to stringify
     * @return Static string representation (no allocation)
     * 
     * @code
     * LOG_DEBUG("Node {} transitioned to {}", 
     *           node.getData()->name, 
     *           nodeStateToString(node.getData()->state));
     * @endcode
     */
    inline const char* nodeStateToString(NodeState state) {
        switch (state) {
            case NodeState::Pending:   return "Pending";
            case NodeState::Ready:     return "Ready";
            case NodeState::Scheduled: return "Scheduled";
            case NodeState::Executing: return "Executing";
            case NodeState::Completed: return "Completed";
            case NodeState::Failed:    return "Failed";
            case NodeState::Cancelled: return "Cancelled";
            case NodeState::Yielded:   return "Yielded";
            default:                   return "Unknown";
        }
    }
    
} // namespace Concurrency
} // namespace Core
} // namespace EntropyEngine

// Hash specialization for NodeHandle - must be defined before any use in unordered_map
namespace std {
    /**
     * @brief Allows NodeHandle to be used as key in unordered_map/unordered_set
     * 
     * This specialization enables you to use NodeHandle as a key in hash-based
     * containers. We hash based on the internal pointer, which is unique per node.
     * 
     * @code
     * std::unordered_map<NodeHandle, int> nodePriorities;
     * nodePriorities[myNode] = 5;  // Works thanks to this specialization
     * @endcode
     */
    template<>
    struct hash<EntropyEngine::Core::Concurrency::NodeHandle> {
        size_t operator()(const EntropyEngine::Core::Concurrency::NodeHandle& handle) const {
            // Use the handle's internal data for hashing
            return hash<uint64_t>{}(reinterpret_cast<uintptr_t>(handle.getData()));
        }
    };
}