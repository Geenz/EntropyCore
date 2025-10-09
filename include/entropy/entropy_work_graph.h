/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file entropy_work_graph.h
 * @brief C API for dependency-based work execution with automatic scheduling
 *
 * WorkGraph orchestrates complex parallel workflows with automatic dependency
 * management. It provides high-level workflow management where tasks are
 * automatically scheduled as their dependencies complete, enabling complex
 * execution patterns without manual scheduling.
 *
 * Key features:
 * - Automatic dependency resolution
 * - Dynamic graph construction during execution
 * - Failure propagation to cancel dependent tasks
 * - Thread-safe operations for concurrent modifications
 * - Main thread execution support for UI/rendering
 * - Yieldable nodes for polling or staged processing
 * - Suspend/resume for execution control
 *
 * Common applications:
 * - Build systems (compile → link → package)
 * - Data pipelines (load → transform → analyze → save)
 * - Game asset processing (texture → compress → pack)
 * - Mixed UI/background workflows
 *
 * Thread Safety: All functions are thread-safe unless otherwise documented.
 */

#pragma once

#include "entropy_concurrency_types.h"
#include "entropy_work_contract_group.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// WorkGraph-Specific Types
// ============================================================================

/**
 * @brief States that a work graph node can be in during its lifecycle
 *
 * These states track a node's progress from creation through completion.
 * State transitions are atomic and thread-safe.
 */
typedef enum EntropyNodeState {
    ENTROPY_NODE_PENDING   = 0,  ///< Waiting for dependencies - can't run yet
    ENTROPY_NODE_READY     = 1,  ///< All dependencies satisfied, waiting for thread
    ENTROPY_NODE_SCHEDULED = 2,  ///< Submitted to WorkContractGroup, in queue
    ENTROPY_NODE_EXECUTING = 3,  ///< Currently running on a worker thread
    ENTROPY_NODE_COMPLETED = 4,  ///< Finished successfully - triggered children
    ENTROPY_NODE_FAILED    = 5,  ///< Exception thrown - children will be cancelled
    ENTROPY_NODE_CANCELLED = 6,  ///< Skipped due to parent failure - never ran
    ENTROPY_NODE_YIELDED   = 7   ///< Suspended execution, will be rescheduled
} EntropyNodeState;

/**
 * @brief Return value from yieldable work functions
 *
 * Allows work functions to control their execution flow. Complete means
 * the work is done, Yield means suspend and reschedule later.
 */
typedef enum EntropyWorkResult {
    ENTROPY_WORK_COMPLETE = 0,   ///< Work is done, proceed to completion
    ENTROPY_WORK_YIELD    = 1    ///< Suspend and reschedule for later execution
} EntropyWorkResult;

// ============================================================================
// Opaque Handle Types
// ============================================================================

/**
 * @brief Opaque handle to a work graph
 *
 * Manages a dependency graph of work nodes with automatic scheduling.
 * Multiple graphs can share the same WorkContractGroup.
 *
 * Lifecycle: Created by entropy_work_graph_create(),
 * destroyed by entropy_work_graph_destroy().
 */
typedef struct entropy_WorkGraph_t* entropy_WorkGraph;

/**
 * @brief Opaque handle to a work graph node
 *
 * Represents a single task within a dependency graph.
 * Handles are stamped with generation for validation.
 *
 * Lifecycle: Created by entropy_work_graph_add_node() or
 * entropy_work_graph_add_yieldable_node().
 */
typedef struct entropy_NodeHandle_t* entropy_NodeHandle;

// ============================================================================
// Callback Types
// ============================================================================

/**
 * @brief Callback signature for yieldable work functions
 *
 * Yieldable work functions can return ENTROPY_WORK_YIELD to suspend
 * execution and be rescheduled later. This enables polling operations
 * or staged processing without blocking threads.
 *
 * @param user_data User-provided context pointer (can be NULL)
 * @return ENTROPY_WORK_COMPLETE when done, ENTROPY_WORK_YIELD to reschedule
 *
 * @code
 * EntropyWorkResult poller(void* user_data) {
 *     DataContext* ctx = (DataContext*)user_data;
 *     if (!data_ready(ctx)) {
 *         return ENTROPY_WORK_YIELD;  // Try again later
 *     }
 *     process_data(ctx);
 *     return ENTROPY_WORK_COMPLETE;
 * }
 * @endcode
 */
typedef EntropyWorkResult (*EntropyYieldableWorkCallback)(void* user_data);

// ============================================================================
// Configuration Structures
// ============================================================================

/**
 * @brief Configuration for work graph creation
 *
 * Controls optional features and tuning parameters.
 */
typedef struct EntropyWorkGraphConfig {
    EntropyBool enable_events;               ///< Enable event bus for monitoring
    EntropyBool enable_state_manager;        ///< Enable advanced state management
    EntropyBool enable_advanced_scheduling;  ///< Enable priority queues, affinity
    size_t expected_node_count;              ///< Expected nodes (for pre-allocation)
    size_t max_deferred_nodes;               ///< Maximum deferred queue size (0 = unlimited)
    size_t max_deferred_processing_iterations; ///< Max iterations when processing deferred
    EntropyBool enable_debug_logging;        ///< Enable verbose debug output
    EntropyBool enable_debug_registration;   ///< Make graph visible in debug tools
} EntropyWorkGraphConfig;

/**
 * @brief Execution result summary from wait()
 *
 * Provides detailed statistics about graph execution after wait() completes.
 */
typedef struct EntropyWaitResult {
    EntropyBool all_completed;    ///< True only if every single node succeeded
    uint32_t dropped_count;       ///< Nodes we couldn't schedule (queue overflow)
    uint32_t failed_count;        ///< Nodes that threw exceptions
    uint32_t completed_count;     ///< Nodes that ran successfully
} EntropyWaitResult;

/**
 * @brief Real-time statistics snapshot
 *
 * Provides a consistent view of graph execution state.
 */
typedef struct EntropyWorkGraphStats {
    uint32_t total_nodes;         ///< Total number of nodes in the graph
    uint32_t completed_nodes;     ///< Successfully finished nodes
    uint32_t failed_nodes;        ///< Nodes that threw exceptions
    uint32_t cancelled_nodes;     ///< Nodes skipped due to parent failure
    uint32_t pending_nodes;       ///< Waiting for dependencies
    uint32_t ready_nodes;         ///< Ready but not yet scheduled
    uint32_t scheduled_nodes;     ///< In the work queue
    uint32_t executing_nodes;     ///< Currently running
    size_t memory_usage;          ///< Approximate memory consumption in bytes
} EntropyWorkGraphStats;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Initialize a work graph config with default values
 *
 * Sets sensible defaults: events disabled, 16 expected nodes, unlimited deferred.
 *
 * @param config Config structure to initialize (required)
 *
 * @code
 * EntropyWorkGraphConfig config;
 * entropy_work_graph_config_init(&config);
 * config.expected_node_count = 1000;  // Customize
 * @endcode
 */
ENTROPY_API void entropy_work_graph_config_init(EntropyWorkGraphConfig* config);

/**
 * @brief Get a human-readable string for a node state
 *
 * @param state The node state
 * @return Static string (do not free)
 */
ENTROPY_API const char* entropy_node_state_to_string(EntropyNodeState state);

/**
 * @brief Get a human-readable string for a work result
 *
 * @param result The work result
 * @return Static string (do not free)
 */
ENTROPY_API const char* entropy_work_result_to_string(EntropyWorkResult result);

// ============================================================================
// WorkGraph Lifecycle
// ============================================================================

/**
 * @brief Creates a work graph backed by a work contract group
 *
 * The graph doesn't own the WorkContractGroup - just uses it to schedule work.
 * Multiple graphs can share the same thread pool.
 *
 * @param work_group Your thread pool for executing work (must outlive the graph)
 * @param status Output parameter for error reporting (required)
 * @return Owned pointer (must call entropy_work_graph_destroy) or NULL on error
 *
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * entropy_WorkGraph graph = entropy_work_graph_create(group, &status);
 * if (status != ENTROPY_OK) {
 *     fprintf(stderr, "Failed to create graph\n");
 *     return -1;
 * }
 * @endcode
 */
ENTROPY_API entropy_WorkGraph entropy_work_graph_create(
    entropy_WorkContractGroup work_group,
    EntropyStatus* status
);

/**
 * @brief Creates a work graph with custom configuration
 *
 * @param work_group Your thread pool for executing work (must outlive the graph)
 * @param config Configuration options
 * @param status Output parameter for error reporting (required)
 * @return Owned pointer (must call entropy_work_graph_destroy) or NULL on error
 *
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer
 *
 * @code
 * EntropyWorkGraphConfig config;
 * entropy_work_graph_config_init(&config);
 * config.enable_events = ENTROPY_TRUE;
 * config.expected_node_count = 1000;
 *
 * entropy_WorkGraph graph = entropy_work_graph_create_with_config(
 *     group, &config, &status
 * );
 * @endcode
 */
ENTROPY_API entropy_WorkGraph entropy_work_graph_create_with_config(
    entropy_WorkContractGroup work_group,
    const EntropyWorkGraphConfig* config,
    EntropyStatus* status
);

/**
 * @brief Destroys a work graph and frees resources
 *
 * Waits for active callbacks before destroying. Safe to destroy with pending
 * work - it continues executing in the WorkContractGroup.
 *
 * Safe to call with NULL (no-op).
 *
 * @param graph The graph to destroy (can be NULL)
 *
 * @threadsafety Thread-safe
 * @ownership Frees the graph
 */
ENTROPY_API void entropy_work_graph_destroy(
    entropy_WorkGraph graph
);

// ============================================================================
// Node Creation
// ============================================================================

/**
 * @brief Adds a task to your workflow
 *
 * Creates a node that waits for dependencies before running. Thread-safe -
 * can add nodes while graph executes.
 *
 * @param graph The work graph (required)
 * @param callback Your task function (required)
 * @param user_data Context pointer passed to callback (can be NULL)
 * @param name Debug name for the node (can be NULL)
 * @param execution_type Where to run (ENTROPY_EXEC_ANY_THREAD or ENTROPY_EXEC_MAIN_THREAD)
 * @param status Output parameter for error reporting (required)
 * @return Handle to the created node, or NULL on error
 *
 * @threadsafety Thread-safe
 * @ownership Returns handle valid until graph destroyed
 *
 * @code
 * void process_data(void* user_data) {
 *     int* value = (int*)user_data;
 *     printf("Processing: %d\n", *value);
 * }
 *
 * int data = 42;
 * entropy_NodeHandle node = entropy_work_graph_add_node(
 *     graph, process_data, &data, "processor",
 *     ENTROPY_EXEC_ANY_THREAD, &status
 * );
 * @endcode
 */
ENTROPY_API entropy_NodeHandle entropy_work_graph_add_node(
    entropy_WorkGraph graph,
    EntropyWorkCallback callback,
    void* user_data,
    const char* name,
    EntropyExecutionType execution_type,
    EntropyStatus* status
);

/**
 * @brief Adds a yieldable task that can suspend and resume execution
 *
 * Creates a node that can yield control back to the scheduler and be
 * rescheduled later. Perfect for polling operations or staged processing.
 *
 * @param graph The work graph (required)
 * @param callback Yieldable function returning EntropyWorkResult (required)
 * @param user_data Context pointer passed to callback (can be NULL)
 * @param name Debug name for the node (can be NULL)
 * @param execution_type Where to run (ENTROPY_EXEC_ANY_THREAD or ENTROPY_EXEC_MAIN_THREAD)
 * @param max_reschedules Maximum reschedule limit (0 = unlimited)
 * @param status Output parameter for error reporting (required)
 * @return Handle to the created node, or NULL on error
 *
 * @threadsafety Thread-safe
 * @ownership Returns handle valid until graph destroyed
 *
 * @code
 * EntropyWorkResult poller(void* user_data) {
 *     if (!data_ready()) {
 *         return ENTROPY_WORK_YIELD;  // Try again later
 *     }
 *     process();
 *     return ENTROPY_WORK_COMPLETE;
 * }
 *
 * entropy_NodeHandle node = entropy_work_graph_add_yieldable_node(
 *     graph, poller, NULL, "poller",
 *     ENTROPY_EXEC_ANY_THREAD, 100, &status
 * );
 * @endcode
 */
ENTROPY_API entropy_NodeHandle entropy_work_graph_add_yieldable_node(
    entropy_WorkGraph graph,
    EntropyYieldableWorkCallback callback,
    void* user_data,
    const char* name,
    EntropyExecutionType execution_type,
    uint32_t max_reschedules,
    EntropyStatus* status
);

// ============================================================================
// Dependency Management
// ============================================================================

/**
 * @brief Wire up your workflow - define execution order
 *
 * Defines that 'to' waits for 'from' to finish. If 'from' fails,
 * 'to' is cancelled. Prevents cycles.
 *
 * @param graph The work graph (required)
 * @param from The prerequisite task (required)
 * @param to The dependent task (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * // Linear pipeline: A → B → C
 * entropy_NodeHandle A = entropy_work_graph_add_node(...);
 * entropy_NodeHandle B = entropy_work_graph_add_node(...);
 * entropy_NodeHandle C = entropy_work_graph_add_node(...);
 *
 * entropy_work_graph_add_dependency(graph, A, B, &status);  // B waits for A
 * entropy_work_graph_add_dependency(graph, B, C, &status);  // C waits for B
 * @endcode
 */
ENTROPY_API void entropy_work_graph_add_dependency(
    entropy_WorkGraph graph,
    entropy_NodeHandle from,
    entropy_NodeHandle to,
    EntropyStatus* status
);

// ============================================================================
// Execution Control
// ============================================================================

/**
 * @brief Start workflow execution
 *
 * Finds and schedules root nodes (nodes with no dependencies).
 * Safe to call multiple times. Thread-safe with dynamic modifications.
 *
 * @param graph The work graph (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * entropy_work_graph_execute(graph, &status);
 * // Graph is now running in the background
 * @endcode
 */
ENTROPY_API void entropy_work_graph_execute(
    entropy_WorkGraph graph,
    EntropyStatus* status
);

/**
 * @brief Suspend graph execution - no new nodes will be scheduled
 *
 * Currently executing nodes will complete, but no new nodes will be
 * scheduled until resume() is called.
 *
 * @param graph The work graph (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * entropy_work_graph_suspend(graph, &status);
 * // Pause execution for some reason
 * entropy_work_graph_resume(graph, &status);
 * @endcode
 */
ENTROPY_API void entropy_work_graph_suspend(
    entropy_WorkGraph graph,
    EntropyStatus* status
);

/**
 * @brief Resume graph execution after suspension
 *
 * Allows scheduling to continue. Nodes that became ready while
 * suspended will be scheduled.
 *
 * @param graph The work graph (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API void entropy_work_graph_resume(
    entropy_WorkGraph graph,
    EntropyStatus* status
);

/**
 * @brief Check if the graph is currently suspended
 *
 * @param graph The work graph (required)
 * @return ENTROPY_TRUE if suspended, ENTROPY_FALSE otherwise
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API EntropyBool entropy_work_graph_is_suspended(
    entropy_WorkGraph graph
);

/**
 * @brief Wait for entire workflow to finish
 *
 * Blocks until all nodes reach terminal state. Returns execution summary.
 *
 * @param graph The work graph (required)
 * @param result Output parameter for wait result (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * EntropyWaitResult result;
 * entropy_work_graph_wait(graph, &result, &status);
 * if (result.all_completed) {
 *     printf("Success! All %u nodes completed\n", result.completed_count);
 * } else {
 *     printf("Failed: %u failures, %u dropped\n",
 *            result.failed_count, result.dropped_count);
 * }
 * @endcode
 */
ENTROPY_API void entropy_work_graph_wait(
    entropy_WorkGraph graph,
    EntropyWaitResult* result,
    EntropyStatus* status
);

/**
 * @brief Quick non-blocking check if workflow is done
 *
 * @param graph The work graph (required)
 * @return ENTROPY_TRUE if all nodes are done, ENTROPY_FALSE if work remains
 *
 * @threadsafety Thread-safe
 *
 * @code
 * // In your game loop
 * if (!entropy_work_graph_is_complete(graph)) {
 *     render_loading_screen();
 * } else {
 *     proceed_to_game();
 * }
 * @endcode
 */
ENTROPY_API EntropyBool entropy_work_graph_is_complete(
    entropy_WorkGraph graph
);

// ============================================================================
// Statistics and Monitoring
// ============================================================================

/**
 * @brief Get a snapshot of graph execution statistics
 *
 * Returns consistent snapshot of all stats. Great for progress bars.
 *
 * @param graph The work graph (required)
 * @param stats Output parameter for statistics (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * EntropyWorkGraphStats stats;
 * entropy_work_graph_get_stats(graph, &stats, &status);
 *
 * float progress = (float)stats.completed_nodes / stats.total_nodes * 100.0f;
 * printf("Progress: %.1f%% (%u/%u nodes)\n",
 *        progress, stats.completed_nodes, stats.total_nodes);
 * @endcode
 */
ENTROPY_API void entropy_work_graph_get_stats(
    entropy_WorkGraph graph,
    EntropyWorkGraphStats* stats,
    EntropyStatus* status
);

/**
 * @brief Get the number of nodes that haven't reached terminal state
 *
 * @param graph The work graph (required)
 * @return Number of pending nodes
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API uint32_t entropy_work_graph_get_pending_count(
    entropy_WorkGraph graph
);

// ============================================================================
// Node Handle Operations
// ============================================================================

/**
 * @brief Test if a node handle is valid
 *
 * @param graph The work graph (required)
 * @param handle The handle to validate (required)
 * @return ENTROPY_TRUE if valid, ENTROPY_FALSE otherwise
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API EntropyBool entropy_node_handle_is_valid(
    entropy_WorkGraph graph,
    entropy_NodeHandle handle
);

/**
 * @brief Get the state of a node
 *
 * @param graph The work graph (required)
 * @param handle The node handle (required)
 * @param status Output parameter for error reporting (required)
 * @return The current node state, or ENTROPY_NODE_PENDING on error
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API EntropyNodeState entropy_node_handle_get_state(
    entropy_WorkGraph graph,
    entropy_NodeHandle handle,
    EntropyStatus* status
);

/**
 * @brief Get the debug name of a node
 *
 * Returns borrowed pointer valid until graph destroyed or node removed.
 *
 * @param graph The work graph (required)
 * @param handle The node handle (required)
 * @return Node name or NULL on error (borrowed pointer, do not free)
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API const char* entropy_node_handle_get_name(
    entropy_WorkGraph graph,
    entropy_NodeHandle handle
);

/**
 * @brief Destroy a node handle
 *
 * Frees the handle wrapper. The node itself remains in the graph.
 * Safe to call with NULL (no-op).
 *
 * @param handle The handle to destroy (can be NULL)
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API void entropy_node_handle_destroy(
    entropy_NodeHandle handle
);

#ifdef __cplusplus
}
#endif
