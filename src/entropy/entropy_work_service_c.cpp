/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "../../include/entropy/entropy_work_service.h"
#include "../Concurrency/WorkService.h"
#include "../Concurrency/WorkContractGroup.h"
#include <new>
#include <limits>

using namespace EntropyEngine::Core::Concurrency;

// ============================================================================
// Internal Helpers
// ============================================================================

namespace {

// Centralized exception translation
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
inline WorkService* to_cpp(entropy_WorkService service) {
    return reinterpret_cast<WorkService*>(service);
}

// Safe cast from C++ object to opaque handle
inline entropy_WorkService to_c(WorkService* service) {
    return reinterpret_cast<entropy_WorkService>(service);
}

// Convert C config to C++ config
WorkService::Config to_cpp_config(const EntropyWorkServiceConfig* c_config) {
    WorkService::Config cpp_config;
    cpp_config.threadCount = c_config->thread_count;
    cpp_config.maxSoftFailureCount = c_config->max_soft_failure_count;
    cpp_config.failureSleepTime = c_config->failure_sleep_time_ns;
    // schedulerConfig uses defaults
    return cpp_config;
}

// Safe cast from opaque group handle to C++ object
inline WorkContractGroup* to_cpp_group(entropy_WorkContractGroup group) {
    return reinterpret_cast<WorkContractGroup*>(group);
}

// Convert C++ MainThreadWorkResult to C struct
void to_c_result(const WorkService::MainThreadWorkResult& cpp_result,
                 EntropyMainThreadWorkResult* c_result) {
    if (!c_result) return;
    c_result->contracts_executed = cpp_result.contractsExecuted;
    c_result->groups_with_work = cpp_result.groupsWithWork;
    c_result->more_work_available = cpp_result.moreWorkAvailable ? ENTROPY_TRUE : ENTROPY_FALSE;
}

} // anonymous namespace

// ============================================================================
// WorkService C API Implementation
// ============================================================================

extern "C" {

entropy_WorkService entropy_work_service_create(
    const EntropyWorkServiceConfig* config,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    *status = ENTROPY_OK;

    if (!config) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        WorkService::Config cpp_config = to_cpp_config(config);
        auto* service = new(std::nothrow) WorkService(cpp_config);
        if (!service) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        return to_c(service);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

void entropy_work_service_destroy(
    entropy_WorkService service
) {
    if (!service) return;

    try {
        WorkService* cpp_service = to_cpp(service);
        delete cpp_service;
    } catch (...) {
        // Swallow exceptions during destruction
    }
}

void entropy_work_service_start(
    entropy_WorkService service,
    EntropyStatus* status
) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!service) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkService* cpp_service = to_cpp(service);
        if (cpp_service->isRunning()) {
            *status = ENTROPY_ERR_ALREADY_RUNNING;
            return;
        }
        cpp_service->start();
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_work_service_request_stop(
    entropy_WorkService service,
    EntropyStatus* status
) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!service) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkService* cpp_service = to_cpp(service);
        cpp_service->requestStop();
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_work_service_wait_for_stop(
    entropy_WorkService service,
    EntropyStatus* status
) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!service) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkService* cpp_service = to_cpp(service);
        cpp_service->waitForStop();
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_work_service_stop(
    entropy_WorkService service,
    EntropyStatus* status
) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!service) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkService* cpp_service = to_cpp(service);
        cpp_service->stop();
    } catch (...) {
        translate_exception(status);
    }
}

EntropyBool entropy_work_service_is_running(
    entropy_WorkService service
) {
    if (!service) return ENTROPY_FALSE;

    try {
        WorkService* cpp_service = to_cpp(service);
        return cpp_service->isRunning() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        return ENTROPY_FALSE;
    }
}

void entropy_work_service_add_group(
    entropy_WorkService service,
    entropy_WorkContractGroup group,
    EntropyStatus* status
) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!service || !group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkService* cpp_service = to_cpp(service);
        WorkContractGroup* cpp_group = to_cpp_group(group);

        auto result = cpp_service->addWorkContractGroup(cpp_group);
        switch (result) {
            case WorkService::GroupOperationStatus::Added:
                *status = ENTROPY_OK;
                break;
            case WorkService::GroupOperationStatus::Exists:
                *status = ENTROPY_ERR_GROUP_EXISTS;
                break;
            case WorkService::GroupOperationStatus::OutOfSpace:
                *status = ENTROPY_ERR_GROUP_FULL;
                break;
            default:
                *status = ENTROPY_ERR_UNKNOWN;
                break;
        }
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_work_service_remove_group(
    entropy_WorkService service,
    entropy_WorkContractGroup group,
    EntropyStatus* status
) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!service || !group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkService* cpp_service = to_cpp(service);
        WorkContractGroup* cpp_group = to_cpp_group(group);

        auto result = cpp_service->removeWorkContractGroup(cpp_group);
        switch (result) {
            case WorkService::GroupOperationStatus::Removed:
                *status = ENTROPY_OK;
                break;
            case WorkService::GroupOperationStatus::NotFound:
                *status = ENTROPY_ERR_GROUP_NOT_FOUND;
                break;
            default:
                *status = ENTROPY_ERR_UNKNOWN;
                break;
        }
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_work_service_clear(
    entropy_WorkService service,
    EntropyStatus* status
) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!service) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkService* cpp_service = to_cpp(service);
        if (cpp_service->isRunning()) {
            *status = ENTROPY_ERR_ALREADY_RUNNING;
            return;
        }
        cpp_service->clear();
    } catch (...) {
        translate_exception(status);
    }
}

size_t entropy_work_service_get_group_count(
    entropy_WorkService service
) {
    if (!service) return 0;

    try {
        WorkService* cpp_service = to_cpp(service);
        return cpp_service->getWorkContractGroupCount();
    } catch (...) {
        return 0;
    }
}

size_t entropy_work_service_get_thread_count(
    entropy_WorkService service
) {
    if (!service) return 0;

    try {
        WorkService* cpp_service = to_cpp(service);
        return cpp_service->getThreadCount();
    } catch (...) {
        return 0;
    }
}

void entropy_work_service_execute_main_thread_work(
    entropy_WorkService service,
    size_t max_contracts,
    EntropyMainThreadWorkResult* result,
    EntropyStatus* status
) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!service) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkService* cpp_service = to_cpp(service);
        size_t limit = (max_contracts == 0) ? std::numeric_limits<size_t>::max() : max_contracts;
        auto cpp_result = cpp_service->executeMainThreadWork(limit);

        if (result) {
            to_c_result(cpp_result, result);
        }
    } catch (...) {
        translate_exception(status);
    }
}

size_t entropy_work_service_execute_main_thread_work_from_group(
    entropy_WorkService service,
    entropy_WorkContractGroup group,
    size_t max_contracts,
    EntropyStatus* status
) {
    if (!status) return 0;
    *status = ENTROPY_OK;

    if (!service || !group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return 0;
    }

    try {
        WorkService* cpp_service = to_cpp(service);
        WorkContractGroup* cpp_group = to_cpp_group(group);
        size_t limit = (max_contracts == 0) ? std::numeric_limits<size_t>::max() : max_contracts;
        return cpp_service->executeMainThreadWork(cpp_group, limit);
    } catch (...) {
        translate_exception(status);
        return 0;
    }
}

EntropyBool entropy_work_service_has_main_thread_work(
    entropy_WorkService service
) {
    if (!service) return ENTROPY_FALSE;

    try {
        WorkService* cpp_service = to_cpp(service);
        return cpp_service->hasMainThreadWork() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        return ENTROPY_FALSE;
    }
}

} // extern "C"
