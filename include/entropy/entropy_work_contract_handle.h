/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file entropy_work_contract_handle.h
 * @brief C API for work contract handles
 *
 * Work contract handles represent individual units of work that can be
 * scheduled for execution. Handles are stamped with owner, index, and
 * generation for safe validation and prevent use-after-free bugs.
 *
 * Thread Safety: All functions in this file are thread-safe unless
 * otherwise documented.
 */

#pragma once

#include "entropy_concurrency_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// WorkContractHandle Operations
// ============================================================================

/**
 * @brief Schedules a contract for execution
 *
 * Transitions the contract from Allocated to Scheduled state. Once scheduled,
 * the contract will be picked up by a worker thread or can be executed
 * manually via the group's execution methods.
 *
 * If the contract is already scheduled, returns ENTROPY_SCHEDULE_ALREADY_SCHEDULED.
 * If the contract is executing, returns ENTROPY_SCHEDULE_EXECUTING.
 *
 * @param handle The work contract handle (required, must be valid)
 * @param status Output parameter for error reporting (required)
 * @return EntropyScheduleResult indicating the outcome
 *
 * @threadsafety Thread-safe
 * @ownership The handle remains owned by caller
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * EntropyScheduleResult result = entropy_work_contract_schedule(handle, &status);
 * if (result == ENTROPY_SCHEDULE_SCHEDULED) {
 *     // Contract is now queued for execution
 * }
 * @endcode
 */
ENTROPY_API EntropyScheduleResult entropy_work_contract_schedule(
    entropy_WorkContractHandle handle,
    EntropyStatus* status
);

/**
 * @brief Attempts to remove a contract from the ready set
 *
 * Transitions the contract from Scheduled back to Allocated state if possible.
 * Fails if the contract is currently executing (too late to cancel).
 *
 * @param handle The work contract handle (required, must be valid)
 * @param status Output parameter for error reporting (required)
 * @return EntropyScheduleResult indicating the outcome
 *
 * @threadsafety Thread-safe
 * @ownership The handle remains owned by caller
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * EntropyScheduleResult result = entropy_work_contract_unschedule(handle, &status);
 * if (result == ENTROPY_SCHEDULE_NOT_SCHEDULED) {
 *     // Successfully cancelled before execution
 * } else if (result == ENTROPY_SCHEDULE_EXECUTING) {
 *     // Too late - already running
 * }
 * @endcode
 */
ENTROPY_API EntropyScheduleResult entropy_work_contract_unschedule(
    entropy_WorkContractHandle handle,
    EntropyStatus* status
);

/**
 * @brief Checks whether a handle still refers to a live contract
 *
 * Validates that the handle's owner, index, and generation match an
 * active slot in the owning group. Returns false if the contract has
 * been released or the handle is invalid.
 *
 * @param handle The work contract handle to validate
 * @return ENTROPY_TRUE if valid, ENTROPY_FALSE otherwise
 *
 * @threadsafety Thread-safe
 *
 * @code
 * if (entropy_work_contract_is_valid(handle)) {
 *     // Safe to schedule or query
 *     entropy_work_contract_schedule(handle, &status);
 * }
 * @endcode
 */
ENTROPY_API EntropyBool entropy_work_contract_is_valid(
    entropy_WorkContractHandle handle
);

/**
 * @brief Immediately frees a contract's slot
 *
 * Clears scheduling state and returns the slot to the free list for reuse.
 * After this call, entropy_work_contract_is_valid() will return false.
 *
 * Safe to call on NULL or already-released handles (no-op).
 *
 * @param handle The work contract handle to release (can be NULL)
 *
 * @threadsafety Thread-safe
 * @ownership Handle becomes invalid after this call
 *
 * @code
 * // Explicit cleanup when work is no longer needed
 * entropy_work_contract_release(handle);
 * handle = NULL; // Good practice
 * @endcode
 */
ENTROPY_API void entropy_work_contract_release(
    entropy_WorkContractHandle handle
);

/**
 * @brief Reports whether the contract is currently scheduled
 *
 * @param handle The work contract handle (required, must be valid)
 * @param status Output parameter for error reporting (required)
 * @return ENTROPY_TRUE if scheduled and waiting for execution, ENTROPY_FALSE otherwise
 *
 * @threadsafety Thread-safe
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * if (entropy_work_contract_is_scheduled(handle, &status)) {
 *     // Contract is queued
 * }
 * @endcode
 */
ENTROPY_API EntropyBool entropy_work_contract_is_scheduled(
    entropy_WorkContractHandle handle,
    EntropyStatus* status
);

/**
 * @brief Reports whether the contract is currently executing
 *
 * @param handle The work contract handle (required, must be valid)
 * @param status Output parameter for error reporting (required)
 * @return ENTROPY_TRUE if actively running, ENTROPY_FALSE otherwise
 *
 * @threadsafety Thread-safe
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * if (entropy_work_contract_is_executing(handle, &status)) {
 *     // Contract is being executed right now
 * }
 * @endcode
 */
ENTROPY_API EntropyBool entropy_work_contract_is_executing(
    entropy_WorkContractHandle handle,
    EntropyStatus* status
);

/**
 * @brief Gets the current state of a contract
 *
 * Returns the contract's lifecycle state (Free, Allocated, Scheduled,
 * Executing, or Completed). Useful for debugging and monitoring.
 *
 * @param handle The work contract handle (required, must be valid)
 * @param status Output parameter for error reporting (required)
 * @return The current contract state, or ENTROPY_CONTRACT_FREE if invalid
 *
 * @threadsafety Thread-safe
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * EntropyContractState state = entropy_work_contract_get_state(handle, &status);
 * if (status == ENTROPY_OK) {
 *     printf("State: %s\n", entropy_contract_state_to_string(state));
 * }
 * @endcode
 */
ENTROPY_API EntropyContractState entropy_work_contract_get_state(
    entropy_WorkContractHandle handle,
    EntropyStatus* status
);

#ifdef __cplusplus
}
#endif
