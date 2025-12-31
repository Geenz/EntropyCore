/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include <new>
#include <stdexcept>

#include "../../include/entropy/entropy_work_contract_handle.h"
#include "../Concurrency/WorkContractHandle.h"

using namespace EntropyEngine::Core::Concurrency;

// ============================================================================
// Internal Helpers
// ============================================================================

namespace
{

// Centralized exception translation for WorkContractHandle operations
void translateException(EntropyStatus* status) {
    if (!status) return;

    try {
        throw;  // Re-throw current exception
    } catch (const std::bad_alloc&) {
        *status = ENTROPY_ERR_NO_MEMORY;
    } catch (const std::invalid_argument&) {
        *status = ENTROPY_ERR_INVALID_ARG;
    } catch (const std::exception&) {
        *status = ENTROPY_ERR_UNKNOWN;
    } catch (...) {
        std::terminate();  // Unknown exception = programming bug
    }
}

// Safe cast from opaque handle to C++ object
inline WorkContractHandle* toCpp(entropy_WorkContractHandle handle) {
    return reinterpret_cast<WorkContractHandle*>(handle);
}

// Convert C++ ScheduleResult to C enum
EntropyScheduleResult toCScheduleResult(ScheduleResult result) {
    switch (result) {
        case ScheduleResult::Scheduled:
            return ENTROPY_SCHEDULE_SCHEDULED;
        case ScheduleResult::AlreadyScheduled:
            return ENTROPY_SCHEDULE_ALREADY_SCHEDULED;
        case ScheduleResult::NotScheduled:
            return ENTROPY_SCHEDULE_NOT_SCHEDULED;
        case ScheduleResult::Executing:
            return ENTROPY_SCHEDULE_EXECUTING;
        case ScheduleResult::Invalid:
        default:
            return ENTROPY_SCHEDULE_INVALID;
    }
}

}  // anonymous namespace

// ============================================================================
// WorkContractHandle C API Implementation
// ============================================================================

extern "C" {

EntropyScheduleResult entropy_work_contract_schedule(entropy_WorkContractHandle handle, EntropyStatus* status) {
    if (!status) return ENTROPY_SCHEDULE_INVALID;
    *status = ENTROPY_OK;

    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_SCHEDULE_INVALID;
    }

    try {
        WorkContractHandle* cppHandle = toCpp(handle);
        ScheduleResult result = cppHandle->schedule();
        return toCScheduleResult(result);
    } catch (...) {
        translateException(status);
        return ENTROPY_SCHEDULE_INVALID;
    }
}

EntropyScheduleResult entropy_work_contract_unschedule(entropy_WorkContractHandle handle, EntropyStatus* status) {
    if (!status) return ENTROPY_SCHEDULE_INVALID;
    *status = ENTROPY_OK;

    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_SCHEDULE_INVALID;
    }

    try {
        WorkContractHandle* cppHandle = toCpp(handle);
        ScheduleResult result = cppHandle->unschedule();
        return toCScheduleResult(result);
    } catch (...) {
        translateException(status);
        return ENTROPY_SCHEDULE_INVALID;
    }
}

EntropyBool entropy_work_contract_is_valid(entropy_WorkContractHandle handle) {
    if (!handle) return ENTROPY_FALSE;

    try {
        WorkContractHandle* cppHandle = toCpp(handle);
        return cppHandle->valid() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        return ENTROPY_FALSE;
    }
}

void entropy_work_contract_release(entropy_WorkContractHandle handle) {
    if (!handle) return;

    try {
        WorkContractHandle* cppHandle = toCpp(handle);
        cppHandle->release();
        // Note: We don't delete the C++ object here because it might still be
        // referenced by the user. The handle becomes invalid but the object remains.
        // This matches the C++ API semantics where handles are value types.
    } catch (...) {
        // Swallow exceptions in release - this is a cleanup path
    }
}

void entropy_work_contract_handle_destroy(entropy_WorkContractHandle handle) {
    if (!handle) return;

    // Delete the heap-allocated wrapper created by entropy_work_contract_group_create_contract
    WorkContractHandle* cppHandle = toCpp(handle);
    delete cppHandle;
}

EntropyBool entropy_work_contract_is_scheduled(entropy_WorkContractHandle handle, EntropyStatus* status) {
    if (!status) return ENTROPY_FALSE;
    *status = ENTROPY_OK;

    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_FALSE;
    }

    try {
        WorkContractHandle* cppHandle = toCpp(handle);
        return cppHandle->isScheduled() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        translateException(status);
        return ENTROPY_FALSE;
    }
}

EntropyBool entropy_work_contract_is_executing(entropy_WorkContractHandle handle, EntropyStatus* status) {
    if (!status) return ENTROPY_FALSE;
    *status = ENTROPY_OK;

    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_FALSE;
    }

    try {
        WorkContractHandle* cppHandle = toCpp(handle);
        return cppHandle->isExecuting() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        translateException(status);
        return ENTROPY_FALSE;
    }
}

EntropyContractState entropy_work_contract_get_state(entropy_WorkContractHandle handle, EntropyStatus* status) {
    if (!status) return ENTROPY_CONTRACT_FREE;
    *status = ENTROPY_OK;

    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_CONTRACT_FREE;
    }

    try {
        WorkContractHandle* cppHandle = toCpp(handle);

        // Query the state through the group since WorkContractHandle
        // doesn't expose getState() directly. We need to get the owner.
        // Actually, looking at the interface, we can determine state from
        // the public methods.

        if (!cppHandle->valid()) {
            return ENTROPY_CONTRACT_FREE;
        } else if (cppHandle->isExecuting()) {
            return ENTROPY_CONTRACT_EXECUTING;
        } else if (cppHandle->isScheduled()) {
            return ENTROPY_CONTRACT_SCHEDULED;
        } else {
            return ENTROPY_CONTRACT_ALLOCATED;
        }
    } catch (...) {
        translateException(status);
        return ENTROPY_CONTRACT_FREE;
    }
}

}  // extern "C"
