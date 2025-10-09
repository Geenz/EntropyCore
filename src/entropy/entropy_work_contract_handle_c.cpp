/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "../../include/entropy/entropy_work_contract_handle.h"
#include "../Concurrency/WorkContractHandle.h"
#include <new>

using namespace EntropyEngine::Core::Concurrency;

// ============================================================================
// Internal Helpers
// ============================================================================

namespace {

// Centralized exception translation for WorkContractHandle operations
void translate_exception(EntropyStatus* status) {
    if (!status) return;

    try {
        throw; // Re-throw current exception
    } catch (const std::bad_alloc&) {
        *status = ENTROPY_ERR_NO_MEMORY;
    } catch (const std::invalid_argument&) {
        *status = ENTROPY_ERR_INVALID_ARG;
    } catch (const std::exception&) {
        *status = ENTROPY_ERR_UNKNOWN;
    } catch (...) {
        std::terminate(); // Unknown exception = programming bug
    }
}

// Safe cast from opaque handle to C++ object
inline WorkContractHandle* to_cpp(entropy_WorkContractHandle handle) {
    return reinterpret_cast<WorkContractHandle*>(handle);
}

// Safe cast from C++ object to opaque handle
inline entropy_WorkContractHandle to_c(WorkContractHandle* handle) {
    return reinterpret_cast<entropy_WorkContractHandle>(handle);
}

// Convert C++ ScheduleResult to C enum
EntropyScheduleResult to_c_schedule_result(ScheduleResult result) {
    switch (result) {
        case ScheduleResult::Scheduled:        return ENTROPY_SCHEDULE_SCHEDULED;
        case ScheduleResult::AlreadyScheduled: return ENTROPY_SCHEDULE_ALREADY_SCHEDULED;
        case ScheduleResult::NotScheduled:     return ENTROPY_SCHEDULE_NOT_SCHEDULED;
        case ScheduleResult::Executing:        return ENTROPY_SCHEDULE_EXECUTING;
        case ScheduleResult::Invalid:          return ENTROPY_SCHEDULE_INVALID;
        default:                               return ENTROPY_SCHEDULE_INVALID;
    }
}

// Convert C++ ContractState to C enum
EntropyContractState to_c_contract_state(ContractState state) {
    switch (state) {
        case ContractState::Free:      return ENTROPY_CONTRACT_FREE;
        case ContractState::Allocated: return ENTROPY_CONTRACT_ALLOCATED;
        case ContractState::Scheduled: return ENTROPY_CONTRACT_SCHEDULED;
        case ContractState::Executing: return ENTROPY_CONTRACT_EXECUTING;
        case ContractState::Completed: return ENTROPY_CONTRACT_COMPLETED;
        default:                       return ENTROPY_CONTRACT_FREE;
    }
}

} // anonymous namespace

// ============================================================================
// WorkContractHandle C API Implementation
// ============================================================================

extern "C" {

EntropyScheduleResult entropy_work_contract_schedule(
    entropy_WorkContractHandle handle,
    EntropyStatus* status
) {
    if (!status) return ENTROPY_SCHEDULE_INVALID;
    *status = ENTROPY_OK;

    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_SCHEDULE_INVALID;
    }

    try {
        WorkContractHandle* cpp_handle = to_cpp(handle);
        ScheduleResult result = cpp_handle->schedule();
        return to_c_schedule_result(result);
    } catch (...) {
        translate_exception(status);
        return ENTROPY_SCHEDULE_INVALID;
    }
}

EntropyScheduleResult entropy_work_contract_unschedule(
    entropy_WorkContractHandle handle,
    EntropyStatus* status
) {
    if (!status) return ENTROPY_SCHEDULE_INVALID;
    *status = ENTROPY_OK;

    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_SCHEDULE_INVALID;
    }

    try {
        WorkContractHandle* cpp_handle = to_cpp(handle);
        ScheduleResult result = cpp_handle->unschedule();
        return to_c_schedule_result(result);
    } catch (...) {
        translate_exception(status);
        return ENTROPY_SCHEDULE_INVALID;
    }
}

EntropyBool entropy_work_contract_is_valid(
    entropy_WorkContractHandle handle
) {
    if (!handle) return ENTROPY_FALSE;

    try {
        WorkContractHandle* cpp_handle = to_cpp(handle);
        return cpp_handle->valid() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        return ENTROPY_FALSE;
    }
}

void entropy_work_contract_release(
    entropy_WorkContractHandle handle
) {
    if (!handle) return;

    try {
        WorkContractHandle* cpp_handle = to_cpp(handle);
        cpp_handle->release();
        // Note: We don't delete the C++ object here because it might still be
        // referenced by the user. The handle becomes invalid but the object remains.
        // This matches the C++ API semantics where handles are value types.
    } catch (...) {
        // Swallow exceptions in release - this is a cleanup path
    }
}

EntropyBool entropy_work_contract_is_scheduled(
    entropy_WorkContractHandle handle,
    EntropyStatus* status
) {
    if (!status) return ENTROPY_FALSE;
    *status = ENTROPY_OK;

    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_FALSE;
    }

    try {
        WorkContractHandle* cpp_handle = to_cpp(handle);
        return cpp_handle->isScheduled() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        translate_exception(status);
        return ENTROPY_FALSE;
    }
}

EntropyBool entropy_work_contract_is_executing(
    entropy_WorkContractHandle handle,
    EntropyStatus* status
) {
    if (!status) return ENTROPY_FALSE;
    *status = ENTROPY_OK;

    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_FALSE;
    }

    try {
        WorkContractHandle* cpp_handle = to_cpp(handle);
        return cpp_handle->isExecuting() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        translate_exception(status);
        return ENTROPY_FALSE;
    }
}

EntropyContractState entropy_work_contract_get_state(
    entropy_WorkContractHandle handle,
    EntropyStatus* status
) {
    if (!status) return ENTROPY_CONTRACT_FREE;
    *status = ENTROPY_OK;

    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_CONTRACT_FREE;
    }

    try {
        WorkContractHandle* cpp_handle = to_cpp(handle);

        // Query the state through the group since WorkContractHandle
        // doesn't expose getState() directly. We need to get the owner.
        // Actually, looking at the interface, we can determine state from
        // the public methods.

        if (!cpp_handle->valid()) {
            return ENTROPY_CONTRACT_FREE;
        } else if (cpp_handle->isExecuting()) {
            return ENTROPY_CONTRACT_EXECUTING;
        } else if (cpp_handle->isScheduled()) {
            return ENTROPY_CONTRACT_SCHEDULED;
        } else {
            return ENTROPY_CONTRACT_ALLOCATED;
        }
    } catch (...) {
        translate_exception(status);
        return ENTROPY_CONTRACT_FREE;
    }
}

} // extern "C"
