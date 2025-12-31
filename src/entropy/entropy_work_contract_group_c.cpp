/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include <limits>
#include <new>
#include <string>

#include "../../include/entropy/entropy_work_contract_group.h"
#include "../Concurrency/WorkContractGroup.h"
#include "../Concurrency/WorkGraphTypes.h"

using namespace EntropyEngine::Core::Concurrency;

// ============================================================================
// Internal Helpers
// ============================================================================

namespace
{

// Centralized exception translation
void translate_exception(EntropyStatus* status) {
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
inline WorkContractGroup* to_cpp(entropy_WorkContractGroup group) {
    return reinterpret_cast<WorkContractGroup*>(group);
}

// Safe cast from C++ object to opaque handle
inline entropy_WorkContractGroup to_c(WorkContractGroup* group) {
    return reinterpret_cast<entropy_WorkContractGroup>(group);
}

// Convert C++ WorkContractHandle to C opaque pointer
inline entropy_WorkContractHandle to_c_handle(WorkContractHandle* cpp_handle) {
    return reinterpret_cast<entropy_WorkContractHandle>(cpp_handle);
}

// Convert C opaque handle to C++ WorkContractHandle pointer
inline WorkContractHandle* to_cpp_handle(entropy_WorkContractHandle handle) {
    return reinterpret_cast<WorkContractHandle*>(handle);
}

// Convert C ExecutionType to C++ enum
ExecutionType to_cpp_execution_type(EntropyExecutionType type) {
    switch (type) {
        case ENTROPY_EXEC_ANY_THREAD:
            return ExecutionType::AnyThread;
        case ENTROPY_EXEC_MAIN_THREAD:
            return ExecutionType::MainThread;
        default:
            return ExecutionType::AnyThread;
    }
}

}  // anonymous namespace

// ============================================================================
// WorkContractGroup C API Implementation
// ============================================================================

extern "C" {

entropy_WorkContractGroup entropy_work_contract_group_create(size_t capacity, const char* name, EntropyStatus* status) {
    if (!status) return nullptr;
    *status = ENTROPY_OK;

    if (capacity == 0) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        std::string group_name = name ? name : "WorkContractGroup";
        auto* group = new (std::nothrow) WorkContractGroup(capacity, group_name);
        if (!group) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        return to_c(group);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

void entropy_work_contract_group_destroy(entropy_WorkContractGroup group) {
    if (!group) return;

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        delete cpp_group;
    } catch (...) {
        // Swallow exceptions during destruction
    }
}

entropy_WorkContractHandle entropy_work_contract_group_create_contract(entropy_WorkContractGroup group,
                                                                       EntropyWorkCallback callback, void* user_data,
                                                                       EntropyExecutionType execution_type,
                                                                       EntropyStatus* status) {
    if (!status) return nullptr;
    *status = ENTROPY_OK;

    if (!group || !callback) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        WorkContractGroup* cpp_group = to_cpp(group);

        // Wrap the C callback in a std::function
        // Capture both callback and user_data by value
        std::function<void()> work = [callback, user_data]() noexcept {
            // Call the C callback with user_data
            callback(user_data);
        };

        ExecutionType cpp_exec_type = to_cpp_execution_type(execution_type);

        // Create the contract
        WorkContractHandle cpp_handle = cpp_group->createContract(work, cpp_exec_type);

        // Check if creation succeeded
        if (!cpp_handle.valid()) {
            *status = ENTROPY_ERR_GROUP_FULL;
            return nullptr;
        }

        // Allocate a C++ handle on the heap to return to C code
        auto* handle_ptr = new (std::nothrow) WorkContractHandle(cpp_handle);
        if (!handle_ptr) {
            // Failed to allocate handle wrapper - need to release the contract
            cpp_handle.release();
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }

        return to_c_handle(handle_ptr);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

void entropy_work_contract_group_wait(entropy_WorkContractGroup group, EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        cpp_group->wait();
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_work_contract_group_stop(entropy_WorkContractGroup group, EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        cpp_group->stop();
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_work_contract_group_resume(entropy_WorkContractGroup group, EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        cpp_group->resume();
    } catch (...) {
        translate_exception(status);
    }
}

EntropyBool entropy_work_contract_group_is_stopping(entropy_WorkContractGroup group) {
    if (!group) return ENTROPY_FALSE;

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        return cpp_group->isStopping() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        return ENTROPY_FALSE;
    }
}

size_t entropy_work_contract_group_capacity(entropy_WorkContractGroup group) {
    if (!group) return 0;

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        return cpp_group->capacity();
    } catch (...) {
        return 0;
    }
}

size_t entropy_work_contract_group_active_count(entropy_WorkContractGroup group) {
    if (!group) return 0;

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        return cpp_group->activeCount();
    } catch (...) {
        return 0;
    }
}

size_t entropy_work_contract_group_scheduled_count(entropy_WorkContractGroup group) {
    if (!group) return 0;

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        return cpp_group->scheduledCount();
    } catch (...) {
        return 0;
    }
}

size_t entropy_work_contract_group_executing_count(entropy_WorkContractGroup group) {
    if (!group) return 0;

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        return cpp_group->executingCount();
    } catch (...) {
        return 0;
    }
}

size_t entropy_work_contract_group_main_thread_scheduled_count(entropy_WorkContractGroup group) {
    if (!group) return 0;

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        return cpp_group->mainThreadScheduledCount();
    } catch (...) {
        return 0;
    }
}

size_t entropy_work_contract_group_main_thread_executing_count(entropy_WorkContractGroup group) {
    if (!group) return 0;

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        return cpp_group->mainThreadExecutingCount();
    } catch (...) {
        return 0;
    }
}

EntropyBool entropy_work_contract_group_has_main_thread_work(entropy_WorkContractGroup group) {
    if (!group) return ENTROPY_FALSE;

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        return cpp_group->hasMainThreadWork() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        return ENTROPY_FALSE;
    }
}

size_t entropy_work_contract_group_execute_all_main_thread_work(entropy_WorkContractGroup group,
                                                                EntropyStatus* status) {
    if (!status) return 0;
    *status = ENTROPY_OK;

    if (!group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return 0;
    }

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        return cpp_group->executeAllMainThreadWork();
    } catch (...) {
        translate_exception(status);
        return 0;
    }
}

size_t entropy_work_contract_group_execute_main_thread_work(entropy_WorkContractGroup group, size_t max_contracts,
                                                            EntropyStatus* status) {
    if (!status) return 0;
    *status = ENTROPY_OK;

    if (!group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return 0;
    }

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        return cpp_group->executeMainThreadWork(max_contracts);
    } catch (...) {
        translate_exception(status);
        return 0;
    }
}

entropy_WorkContractHandle entropy_work_contract_group_select_for_execution(entropy_WorkContractGroup group,
                                                                            uint64_t* bias, EntropyStatus* status) {
    if (!status) return nullptr;
    *status = ENTROPY_OK;

    if (!group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        WorkContractGroup* cpp_group = to_cpp(group);

        WorkContractHandle cpp_handle;
        if (bias) {
            cpp_handle = cpp_group->selectForExecution(std::ref(*bias));
        } else {
            cpp_handle = cpp_group->selectForExecution();
        }

        if (!cpp_handle.valid()) {
            return nullptr;
        }

        // Allocate a C++ handle on the heap
        auto* handle_ptr = new (std::nothrow) WorkContractHandle(cpp_handle);
        if (!handle_ptr) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }

        return to_c_handle(handle_ptr);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

void entropy_work_contract_group_execute_contract(entropy_WorkContractGroup group, entropy_WorkContractHandle handle,
                                                  EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!group || !handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        WorkContractHandle* cpp_handle = to_cpp_handle(handle);
        cpp_group->executeContract(*cpp_handle);
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_work_contract_group_complete_execution(entropy_WorkContractGroup group, entropy_WorkContractHandle handle,
                                                    EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!group || !handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkContractGroup* cpp_group = to_cpp(group);
        WorkContractHandle* cpp_handle = to_cpp_handle(handle);
        cpp_group->completeExecution(*cpp_handle);

        // After completion, the handle is likely invalid, so we can free it
        // Actually, let's not auto-free here because the user might want to check valid()
        // They should manually manage the handle lifetime
    } catch (...) {
        translate_exception(status);
    }
}

}  // extern "C"
