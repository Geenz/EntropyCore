/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file entropy_concurrency_types.h
 * @brief C API types and enums for EntropyCore concurrency primitives
 *
 * This header defines the fundamental types, enums, and constants used by
 * the EntropyCore C concurrency API. All types are C89-compatible and
 * designed for stable ABI across language boundaries.
 */

#pragma once

#include "../../src/Core/entropy_c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Concurrency-Specific Status Codes
// ============================================================================

/**
 * @brief Extended status codes for concurrency operations
 *
 * These extend the base EntropyStatus enum with concurrency-specific errors.
 */
#define ENTROPY_ERR_ALREADY_SCHEDULED ((EntropyStatus)100)  ///< Contract is already scheduled
#define ENTROPY_ERR_EXECUTING ((EntropyStatus)101)          ///< Contract is currently executing
#define ENTROPY_ERR_GROUP_FULL ((EntropyStatus)102)         ///< Work group has reached capacity
#define ENTROPY_ERR_NOT_SCHEDULED ((EntropyStatus)103)      ///< Contract is not scheduled
#define ENTROPY_ERR_ALREADY_RUNNING ((EntropyStatus)104)    ///< Service is already running
#define ENTROPY_ERR_NOT_RUNNING ((EntropyStatus)105)        ///< Service is not running
#define ENTROPY_ERR_GROUP_EXISTS ((EntropyStatus)106)       ///< Group is already registered
#define ENTROPY_ERR_GROUP_NOT_FOUND ((EntropyStatus)107)    ///< Group was not found

// ============================================================================
// Contract State
// ============================================================================

/**
 * @brief States that a work contract can be in during its lifecycle
 *
 * These states track a contract's progress from allocation through completion.
 * State transitions are atomic and thread-safe.
 */
typedef enum EntropyContractState
{
    ENTROPY_CONTRACT_FREE = 0,       ///< Contract slot is available for allocation
    ENTROPY_CONTRACT_ALLOCATED = 1,  ///< Contract has been allocated but not scheduled
    ENTROPY_CONTRACT_SCHEDULED = 2,  ///< Contract is scheduled and ready for execution
    ENTROPY_CONTRACT_EXECUTING = 3,  ///< Contract is currently being executed
    ENTROPY_CONTRACT_COMPLETED = 4   ///< Contract has completed execution
} EntropyContractState;

// ============================================================================
// Schedule Result
// ============================================================================

/**
 * @brief Result of schedule/unschedule operations
 *
 * Indicates the outcome of attempting to change a contract's scheduling state.
 */
typedef enum EntropyScheduleResult
{
    ENTROPY_SCHEDULE_SCHEDULED = 0,          ///< Contract is now scheduled (successful schedule operation)
    ENTROPY_SCHEDULE_ALREADY_SCHEDULED = 1,  ///< Contract was already scheduled (schedule operation failed)
    ENTROPY_SCHEDULE_NOT_SCHEDULED = 2,      ///< Contract is not scheduled (successful unschedule operation)
    ENTROPY_SCHEDULE_EXECUTING = 3,          ///< Cannot modify - currently executing
    ENTROPY_SCHEDULE_INVALID = 4             ///< Invalid handle provided
} EntropyScheduleResult;

// ============================================================================
// Execution Type
// ============================================================================

/**
 * @brief Execution context for work contracts
 *
 * Determines where a contract is allowed to execute. Use MainThread for
 * work that must run on the main thread (UI updates, rendering setup, etc.).
 */
typedef enum EntropyExecutionType
{
    ENTROPY_EXEC_ANY_THREAD = 0,  ///< Runs on any worker thread from the pool
    ENTROPY_EXEC_MAIN_THREAD = 1  ///< Must run on the main/UI thread
} EntropyExecutionType;

// ============================================================================
// Opaque Handle Types
// ============================================================================

/**
 * @brief Opaque handle to a work contract
 *
 * Represents a single unit of work that can be scheduled and executed.
 * Handles are stamped with owner, index, and generation for validation.
 *
 * Lifecycle: Created by entropy_work_contract_group_create_contract(),
 * invalidated when released or after execution completes.
 */
typedef struct entropy_WorkContractHandle_t* entropy_WorkContractHandle;

/**
 * @brief Opaque handle to a work contract group
 *
 * A work contract group manages a pool of work contracts with lock-free
 * scheduling. Multiple threads can safely schedule and select work.
 *
 * Lifecycle: Created by entropy_work_contract_group_create(),
 * destroyed by entropy_work_contract_group_destroy().
 */
typedef struct entropy_WorkContractGroup_t* entropy_WorkContractGroup;

/**
 * @brief Opaque handle to a work service
 *
 * A work service is a thread pool that executes contracts from one or more
 * work contract groups using a pluggable scheduling strategy.
 *
 * Lifecycle: Created by entropy_work_service_create(),
 * destroyed by entropy_work_service_destroy().
 */
typedef struct entropy_WorkService_t* entropy_WorkService;

// ============================================================================
// Callback Types
// ============================================================================

/**
 * @brief Callback signature for work functions
 *
 * Work functions receive a user_data pointer for closure state.
 * The callback must be thread-safe and should not throw exceptions.
 *
 * @param user_data User-provided context pointer (can be NULL)
 *
 * @code
 * void my_work_function(void* user_data) {
 *     MyContext* ctx = (MyContext*)user_data;
 *     // Do work...
 * }
 * @endcode
 */
typedef void (*EntropyWorkCallback)(void* user_data);

// ============================================================================
// Configuration Structures
// ============================================================================

/**
 * @brief Configuration for work service creation
 *
 * Controls thread pool size and scheduling behavior.
 */
typedef struct EntropyWorkServiceConfig
{
    uint32_t thread_count;          ///< Worker thread count (0 = use all CPU cores)
    size_t max_soft_failure_count;  ///< Number of selection failures before sleeping
    size_t failure_sleep_time_ns;   ///< Sleep duration in nanoseconds when no work found
} EntropyWorkServiceConfig;

/**
 * @brief Result of main thread work execution
 *
 * Provides detailed statistics about main thread work execution.
 */
typedef struct EntropyMainThreadWorkResult
{
    size_t contracts_executed;        ///< Number of contracts actually executed
    size_t groups_with_work;          ///< Number of groups that had work available
    EntropyBool more_work_available;  ///< Whether there's more work that could be executed
} EntropyMainThreadWorkResult;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Get a human-readable string for a contract state
 *
 * @param state The contract state
 * @return Static string (do not free)
 */
ENTROPY_API const char* entropy_contract_state_to_string(EntropyContractState state);

/**
 * @brief Get a human-readable string for a schedule result
 *
 * @param result The schedule result
 * @return Static string (do not free)
 */
ENTROPY_API const char* entropy_schedule_result_to_string(EntropyScheduleResult result);

/**
 * @brief Get a human-readable string for an execution type
 *
 * @param type The execution type
 * @return Static string (do not free)
 */
ENTROPY_API const char* entropy_execution_type_to_string(EntropyExecutionType type);

/**
 * @brief Initialize a work service config with default values
 *
 * Sets sensible defaults: auto-detect thread count, 5 soft failures, 1ns sleep.
 *
 * @param config Config structure to initialize (required)
 */
ENTROPY_API void entropy_work_service_config_init(EntropyWorkServiceConfig* config);

#ifdef __cplusplus
}
#endif
