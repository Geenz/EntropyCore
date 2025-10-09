/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file entropy_work_contract_group.h
 * @brief C API for work contract groups
 *
 * A work contract group is a lock-free pool of work contracts with concurrent
 * scheduling primitives. It manages thousands of tasks without locks or blocking,
 * making it ideal for game engines, parallel processing systems, and high-throughput
 * work management.
 *
 * Key features:
 * - Lock-free contract scheduling and selection
 * - Generation-based handles prevent use-after-free bugs
 * - Immediate resource cleanup on completion
 * - Statistical monitoring (active/scheduled counts)
 * - Wait functionality for synchronization points
 * - Main thread work separation for UI/rendering tasks
 *
 * Thread Safety: All functions in this file are thread-safe unless
 * otherwise documented.
 */

#pragma once

#include "entropy_concurrency_types.h"
#include "entropy_work_contract_handle.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// WorkContractGroup Lifecycle
// ============================================================================

/**
 * @brief Creates a new work contract group with specified capacity
 *
 * Pre-allocates all data structures for lock-free operation. Choose capacity
 * based on peak concurrent load. Typical values: 1024-8192 for game engines,
 * 512-2048 for background processing.
 *
 * @param capacity Maximum number of contracts (must be > 0)
 * @param name Optional debug name (can be NULL for auto-generated name)
 * @param status Output parameter for error reporting (required)
 * @return Owned pointer (must call entropy_work_contract_group_destroy) or NULL on error
 *
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * entropy_WorkContractGroup group = entropy_work_contract_group_create(
 *     2048, "FrameWork", &status
 * );
 * if (status != ENTROPY_OK) {
 *     fprintf(stderr, "Failed to create group: %s\n",
 *             entropy_status_to_string(status));
 *     return -1;
 * }
 * @endcode
 */
ENTROPY_API entropy_WorkContractGroup entropy_work_contract_group_create(
    size_t capacity,
    const char* name,
    EntropyStatus* status
);

/**
 * @brief Destroys a work contract group and frees resources
 *
 * Follows strict destruction protocol to prevent deadlocks:
 * 1. Stops accepting new work selection
 * 2. Waits for all executing work to complete
 * 3. Unschedules and releases all remaining contracts
 * 4. Notifies any registered concurrency provider
 *
 * Safe to call with NULL (no-op).
 *
 * CRITICAL: All work must complete before destruction. If worker threads
 * are still selecting work, they must be stopped first.
 *
 * @param group The group to destroy (can be NULL)
 *
 * @threadsafety Thread-safe, but caller must ensure no concurrent selections
 * @ownership Frees the group and invalidates all handles
 *
 * @code
 * // Stop workers, wait, then destroy
 * entropy_work_service_stop(service, &status);
 * entropy_work_contract_group_destroy(group);
 * @endcode
 */
ENTROPY_API void entropy_work_contract_group_destroy(
    entropy_WorkContractGroup group
);

// ============================================================================
// Contract Creation
// ============================================================================

/**
 * @brief Creates a new work contract with the given work function
 *
 * Allocates a contract slot from the group's pool and stores the callback.
 * The contract starts in Allocated state - call entropy_work_contract_schedule()
 * to queue it for execution.
 *
 * @param group The work contract group (required)
 * @param callback The work function to execute (required)
 * @param user_data Context pointer passed to callback (can be NULL)
 * @param execution_type Where this contract should execute (ENTROPY_EXEC_ANY_THREAD or ENTROPY_EXEC_MAIN_THREAD)
 * @param status Output parameter for error reporting (required)
 * @return Handle to the created contract, or NULL if group is full
 *
 * @threadsafety Thread-safe
 * @ownership Returns handle that becomes invalid after release or execution
 *
 * @code
 * typedef struct {
 *     int task_id;
 *     float* data;
 * } TaskContext;
 *
 * void process_task(void* user_data) {
 *     TaskContext* ctx = (TaskContext*)user_data;
 *     // Process data...
 * }
 *
 * EntropyStatus status = ENTROPY_OK;
 * TaskContext* ctx = malloc(sizeof(TaskContext));
 * ctx->task_id = 42;
 * ctx->data = my_data;
 *
 * entropy_WorkContractHandle handle = entropy_work_contract_group_create_contract(
 *     group, process_task, ctx, ENTROPY_EXEC_ANY_THREAD, &status
 * );
 *
 * if (handle != NULL) {
 *     entropy_work_contract_schedule(handle, &status);
 * }
 * @endcode
 */
ENTROPY_API entropy_WorkContractHandle entropy_work_contract_group_create_contract(
    entropy_WorkContractGroup group,
    EntropyWorkCallback callback,
    void* user_data,
    EntropyExecutionType execution_type,
    EntropyStatus* status
);

// ============================================================================
// Synchronization
// ============================================================================

/**
 * @brief Waits for all scheduled and executing contracts to complete
 *
 * Blocks until all work finishes. Includes both scheduled (queued) and
 * executing (running) contracts. Does not prevent new work from being created.
 *
 * Useful for synchronization points like frame boundaries, level transitions,
 * or shutdown sequences.
 *
 * @param group The work contract group (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * // Submit a batch of work
 * for (int i = 0; i < 100; i++) {
 *     entropy_WorkContractHandle h = entropy_work_contract_group_create_contract(
 *         group, process_item, &items[i], ENTROPY_EXEC_ANY_THREAD, &status
 *     );
 *     entropy_work_contract_schedule(h, &status);
 * }
 *
 * // Wait for all work to complete
 * entropy_work_contract_group_wait(group, &status);
 * printf("All work finished!\n");
 * @endcode
 */
ENTROPY_API void entropy_work_contract_group_wait(
    entropy_WorkContractGroup group,
    EntropyStatus* status
);

/**
 * @brief Stops the group from accepting new work selections
 *
 * Prevents new work selection via selectForExecution(). Executing work
 * continues normally. Thread-safe and can be called multiple times.
 *
 * Typically used during shutdown or when pausing a subsystem.
 *
 * @param group The work contract group (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * // Graceful shutdown
 * entropy_work_contract_group_stop(group, &status);
 * entropy_work_contract_group_wait(group, &status);
 * entropy_work_contract_group_destroy(group);
 * @endcode
 */
ENTROPY_API void entropy_work_contract_group_stop(
    entropy_WorkContractGroup group,
    EntropyStatus* status
);

/**
 * @brief Resumes the group to allow new work selections
 *
 * Clears the stopping flag set by entropy_work_contract_group_stop().
 * Work selection will resume normally. Does not automatically notify
 * waiting threads - they must check again.
 *
 * @param group The work contract group (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * // Pause and resume
 * entropy_work_contract_group_stop(group, &status);
 * // Do some setup...
 * entropy_work_contract_group_resume(group, &status);
 * @endcode
 */
ENTROPY_API void entropy_work_contract_group_resume(
    entropy_WorkContractGroup group,
    EntropyStatus* status
);

/**
 * @brief Checks if the group is in the process of stopping
 *
 * @param group The work contract group (required)
 * @return ENTROPY_TRUE if stop() has been called, ENTROPY_FALSE otherwise
 *
 * @threadsafety Thread-safe
 *
 * @code
 * if (entropy_work_contract_group_is_stopping(group)) {
 *     // Finish current work and exit
 * }
 * @endcode
 */
ENTROPY_API EntropyBool entropy_work_contract_group_is_stopping(
    entropy_WorkContractGroup group
);

// ============================================================================
// Statistics and Monitoring
// ============================================================================

/**
 * @brief Gets the maximum capacity of the group
 *
 * @param group The work contract group (required)
 * @return Maximum number of contracts this group can handle
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API size_t entropy_work_contract_group_capacity(
    entropy_WorkContractGroup group
);

/**
 * @brief Gets the number of currently allocated contracts
 *
 * Contracts are allocated from when they're created until they're released.
 *
 * @param group The work contract group (required)
 * @return Number of contracts that have been created but not yet released
 *
 * @threadsafety Thread-safe
 *
 * @code
 * size_t used = entropy_work_contract_group_active_count(group);
 * size_t capacity = entropy_work_contract_group_capacity(group);
 * printf("Using %zu of %zu slots (%.1f%%)\n",
 *        used, capacity, (used * 100.0) / capacity);
 * @endcode
 */
ENTROPY_API size_t entropy_work_contract_group_active_count(
    entropy_WorkContractGroup group
);

/**
 * @brief Gets the number of contracts currently scheduled for execution
 *
 * Scheduled means queued and waiting for a worker thread to pick them up.
 * Does not include executing or allocated-but-not-scheduled contracts.
 *
 * @param group The work contract group (required)
 * @return Number of contracts in the ready queue
 *
 * @threadsafety Thread-safe
 *
 * @code
 * size_t queued = entropy_work_contract_group_scheduled_count(group);
 * if (queued > 100) {
 *     printf("Warning: Work backlog is building up\n");
 * }
 * @endcode
 */
ENTROPY_API size_t entropy_work_contract_group_scheduled_count(
    entropy_WorkContractGroup group
);

/**
 * @brief Gets the number of contracts currently executing
 *
 * Useful for thread scheduling and load balancing decisions.
 *
 * @param group The work contract group (required)
 * @return The number of currently executing contracts
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API size_t entropy_work_contract_group_executing_count(
    entropy_WorkContractGroup group
);

/**
 * @brief Gets the number of main thread contracts currently scheduled
 *
 * @param group The work contract group (required)
 * @return Number of main thread contracts waiting for execution
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API size_t entropy_work_contract_group_main_thread_scheduled_count(
    entropy_WorkContractGroup group
);

/**
 * @brief Gets the number of main thread contracts currently executing
 *
 * @param group The work contract group (required)
 * @return Number of main thread contracts being executed
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API size_t entropy_work_contract_group_main_thread_executing_count(
    entropy_WorkContractGroup group
);

/**
 * @brief Checks if there are any main thread contracts ready to execute
 *
 * Quick check to avoid unnecessary calls to executeMainThreadWork().
 *
 * @param group The work contract group (required)
 * @return ENTROPY_TRUE if main thread work is available
 *
 * @threadsafety Thread-safe
 *
 * @code
 * // Only pump if there's work
 * if (entropy_work_contract_group_has_main_thread_work(group)) {
 *     entropy_work_contract_group_execute_main_thread_work(group, 5, &status);
 * }
 * @endcode
 */
ENTROPY_API EntropyBool entropy_work_contract_group_has_main_thread_work(
    entropy_WorkContractGroup group
);

// ============================================================================
// Main Thread Execution
// ============================================================================

/**
 * @brief Executes all main thread targeted work contracts
 *
 * Drains all main thread work at once. Use this when you want to process
 * all pending main thread work in a single call. Must be called from the
 * main thread.
 *
 * @param group The work contract group (required)
 * @param status Output parameter for error reporting (required)
 * @return Number of contracts executed
 *
 * @threadsafety NOT thread-safe - must be called from main thread only
 *
 * @code
 * // In your main update loop
 * EntropyStatus status = ENTROPY_OK;
 * size_t executed = entropy_work_contract_group_execute_all_main_thread_work(
 *     group, &status
 * );
 * if (executed > 0) {
 *     printf("Processed %zu main thread tasks\n", executed);
 * }
 * @endcode
 */
ENTROPY_API size_t entropy_work_contract_group_execute_all_main_thread_work(
    entropy_WorkContractGroup group,
    EntropyStatus* status
);

/**
 * @brief Executes main thread targeted work contracts with a limit
 *
 * Use when you need to bound main thread work per frame/iteration to
 * maintain responsiveness. Prevents blocking the main thread for too long.
 * Must be called from the main thread.
 *
 * @param group The work contract group (required)
 * @param max_contracts Maximum number of contracts to execute
 * @param status Output parameter for error reporting (required)
 * @return Number of contracts actually executed (may be less than max_contracts)
 *
 * @threadsafety NOT thread-safe - must be called from main thread only
 *
 * @code
 * // Limit main thread work to maintain 60 FPS
 * EntropyStatus status = ENTROPY_OK;
 * while (running) {
 *     // Execute at most 5 tasks per frame
 *     entropy_work_contract_group_execute_main_thread_work(group, 5, &status);
 *     render_frame();
 * }
 * @endcode
 */
ENTROPY_API size_t entropy_work_contract_group_execute_main_thread_work(
    entropy_WorkContractGroup group,
    size_t max_contracts,
    EntropyStatus* status
);

// ============================================================================
// Advanced Execution Control (for custom executors)
// ============================================================================

/**
 * @brief Selects a scheduled contract for execution
 *
 * Atomically transitions a contract from Scheduled to Executing state.
 * This is an advanced API - most users should use WorkService instead
 * of implementing custom executors.
 *
 * The bias parameter provides fair work distribution across multiple
 * selectors. Pass NULL if you don't need bias tracking.
 *
 * @param group The work contract group (required)
 * @param bias Optional selection bias pointer for fair distribution (can be NULL)
 * @param status Output parameter for error reporting (required)
 * @return Handle to an executing contract, or NULL if none available
 *
 * @threadsafety Thread-safe
 *
 * @code
 * // Custom executor loop
 * EntropyStatus status = ENTROPY_OK;
 * uint64_t bias = 0;
 * while (running) {
 *     entropy_WorkContractHandle h = entropy_work_contract_group_select_for_execution(
 *         group, &bias, &status
 *     );
 *     if (h == NULL) break; // No more work
 *
 *     entropy_work_contract_group_execute_contract(group, h, &status);
 *     entropy_work_contract_group_complete_execution(group, h, &status);
 * }
 * @endcode
 */
ENTROPY_API entropy_WorkContractHandle entropy_work_contract_group_select_for_execution(
    entropy_WorkContractGroup group,
    uint64_t* bias,
    EntropyStatus* status
);

/**
 * @brief Executes the work function of a contract
 *
 * Only call on contracts returned by select_for_execution().
 * This is an advanced API - most users should use WorkService.
 *
 * @param group The work contract group (required)
 * @param handle Handle to the contract to execute (must be in Executing state)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe, but each handle can only be executed once
 */
ENTROPY_API void entropy_work_contract_group_execute_contract(
    entropy_WorkContractGroup group,
    entropy_WorkContractHandle handle,
    EntropyStatus* status
);

/**
 * @brief Completes execution and cleans up a contract
 *
 * Must be called after execute_contract() to complete the lifecycle.
 * This is an advanced API - most users should use WorkService.
 *
 * @param group The work contract group (required)
 * @param handle Handle to the contract that finished executing
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 */
ENTROPY_API void entropy_work_contract_group_complete_execution(
    entropy_WorkContractGroup group,
    entropy_WorkContractHandle handle,
    EntropyStatus* status
);

#ifdef __cplusplus
}
#endif
