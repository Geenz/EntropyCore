/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include <thread>

#include "../../include/entropy/entropy_concurrency_types.h"

// ============================================================================
// Helper String Conversions
// ============================================================================

extern "C" {

const char* entropy_contract_state_to_string(EntropyContractState state) {
    switch (state) {
        case ENTROPY_CONTRACT_FREE:
            return "Free";
        case ENTROPY_CONTRACT_ALLOCATED:
            return "Allocated";
        case ENTROPY_CONTRACT_SCHEDULED:
            return "Scheduled";
        case ENTROPY_CONTRACT_EXECUTING:
            return "Executing";
        case ENTROPY_CONTRACT_COMPLETED:
            return "Completed";
        default:
            return "Unknown";
    }
}

const char* entropy_schedule_result_to_string(EntropyScheduleResult result) {
    switch (result) {
        case ENTROPY_SCHEDULE_SCHEDULED:
            return "Scheduled";
        case ENTROPY_SCHEDULE_ALREADY_SCHEDULED:
            return "AlreadyScheduled";
        case ENTROPY_SCHEDULE_NOT_SCHEDULED:
            return "NotScheduled";
        case ENTROPY_SCHEDULE_EXECUTING:
            return "Executing";
        case ENTROPY_SCHEDULE_INVALID:
            return "Invalid";
        default:
            return "Unknown";
    }
}

const char* entropy_execution_type_to_string(EntropyExecutionType type) {
    switch (type) {
        case ENTROPY_EXEC_ANY_THREAD:
            return "AnyThread";
        case ENTROPY_EXEC_MAIN_THREAD:
            return "MainThread";
        default:
            return "Unknown";
    }
}

void entropy_work_service_config_init(EntropyWorkServiceConfig* config) {
    if (!config) return;

    config->thread_count = 0;  // 0 = auto-detect
    config->max_soft_failure_count = 5;
    config->failure_sleep_time_ns = 1;
}

}  // extern "C"
