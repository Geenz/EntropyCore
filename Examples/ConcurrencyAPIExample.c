/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file ConcurrencyAPIExample.c
 * @brief Example demonstrating the C API for EntropyCore concurrency primitives
 *
 * This example shows how to use the C API to create work groups, submit tasks,
 * and execute them using a work service (thread pool), along with the C logging API.
 */

#include <entropy/entropy_concurrency_types.h>
#include <entropy/entropy_work_contract_group.h>
#include <entropy/entropy_work_contract_handle.h>
#include <entropy/entropy_work_service.h>
#include <Logging/CLogger.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define usleep(x) Sleep((x) / 1000)  // usleep is microseconds, Sleep is milliseconds
#else
#include <unistd.h>
#endif

// Example work context
typedef struct {
    int task_id;
    const char* message;
} TaskContext;

// Re-entrant work context (task spawns children)
typedef struct {
    entropy_WorkContractGroup group;  // Need group reference to schedule from within
    int depth;                        // Recursion depth
    int max_depth;                    // Maximum depth
    int task_id;                      // Task identifier
} ReentrantContext;

// Example work callback
void example_work(void* user_data) {
    TaskContext* ctx = (TaskContext*)user_data;
    ENTROPY_LOG_INFO_CAT_F("WorkerThread",
        "Task %d: %s (executing on worker thread)",
        ctx->task_id, ctx->message);

    // Simulate some work
    usleep(10000); // 10ms

    ENTROPY_LOG_DEBUG_CAT_F("WorkerThread",
        "Task %d completed", ctx->task_id);
}

// Main thread work callback
void main_thread_work(void* user_data) {
    TaskContext* ctx = (TaskContext*)user_data;
    ENTROPY_LOG_INFO_CAT_F("MainThread",
        "Task %d: %s (executing on MAIN THREAD)",
        ctx->task_id, ctx->message);
}

// Re-entrant work callback (spawns child tasks)
void reentrant_work(void* user_data) {
    ReentrantContext* ctx = (ReentrantContext*)user_data;

    ENTROPY_LOG_INFO_CAT_F("Reentrant",
        "Task %d at depth %d (max: %d)",
        ctx->task_id, ctx->depth, ctx->max_depth);

    // If we haven't reached max depth, spawn two child tasks
    if (ctx->depth < ctx->max_depth) {
        EntropyStatus status = ENTROPY_OK;

        // Allocate contexts for child tasks (will be freed by children or at end)
        ReentrantContext* left_child = malloc(sizeof(ReentrantContext));
        ReentrantContext* right_child = malloc(sizeof(ReentrantContext));

        if (left_child && right_child) {
            // Setup left child
            left_child->group = ctx->group;
            left_child->depth = ctx->depth + 1;
            left_child->max_depth = ctx->max_depth;
            left_child->task_id = ctx->task_id * 2;

            // Setup right child
            right_child->group = ctx->group;
            right_child->depth = ctx->depth + 1;
            right_child->max_depth = ctx->max_depth;
            right_child->task_id = ctx->task_id * 2 + 1;

            ENTROPY_LOG_DEBUG_CAT_F("Reentrant",
                "Task %d spawning children %d and %d",
                ctx->task_id, left_child->task_id, right_child->task_id);

            // RE-ENTRANT SCHEDULING: Create and schedule child tasks from within this task
            entropy_WorkContractHandle left = entropy_work_contract_group_create_contract(
                ctx->group, reentrant_work, left_child, ENTROPY_EXEC_ANY_THREAD, &status
            );

            if (status == ENTROPY_OK) {
                entropy_work_contract_schedule(left, &status);
            }
            entropy_work_contract_handle_destroy(left);

            entropy_WorkContractHandle right = entropy_work_contract_group_create_contract(
                ctx->group, reentrant_work, right_child, ENTROPY_EXEC_ANY_THREAD, &status
            );

            if (status == ENTROPY_OK) {
                entropy_work_contract_schedule(right, &status);
            }
            entropy_work_contract_handle_destroy(right);
        }
    } else {
        ENTROPY_LOG_DEBUG_CAT_F("Reentrant",
            "Task %d reached max depth, completing as leaf",
            ctx->task_id);
    }

    // Free our own context (we're done with it)
    free(ctx);
}

int main(void) {
    EntropyStatus status = ENTROPY_OK;

    ENTROPY_LOG_INFO_CAT_F("Example", "=== EntropyCore C API Example ===");
    ENTROPY_LOG_INFO_CAT_F("Example", "Demonstrating C API for concurrency + logging");

    // 1. Create a work contract group
    ENTROPY_LOG_INFO_CAT_F("Example", "Step 1: Creating work contract group (capacity: 1024)...");
    entropy_WorkContractGroup group = entropy_work_contract_group_create(
        1024, "ExampleGroup", &status
    );
    if (status != ENTROPY_OK) {
        ENTROPY_LOG_ERROR_CAT_F("Example",
            "Failed to create group: %s", entropy_status_to_string(status));
        return 1;
    }
    ENTROPY_LOG_INFO_CAT_F("Example", "Group created successfully");

    // 2. Create a work service (thread pool)
    ENTROPY_LOG_INFO_CAT_F("Example", "Step 2: Creating work service (4 threads)...");
    EntropyWorkServiceConfig config;
    entropy_work_service_config_init(&config);
    config.thread_count = 4;

    entropy_WorkService service = entropy_work_service_create(&config, &status);
    if (status != ENTROPY_OK) {
        ENTROPY_LOG_ERROR_CAT_F("Example",
            "Failed to create service: %s", entropy_status_to_string(status));
        entropy_work_contract_group_destroy(group);
        return 1;
    }
    ENTROPY_LOG_INFO_CAT_F("Example", "Service created with %zu threads",
           entropy_work_service_get_thread_count(service));

    // 3. Register the group with the service
    ENTROPY_LOG_INFO_CAT_F("Example", "Step 3: Registering group with service...");
    entropy_work_service_add_group(service, group, &status);
    if (status != ENTROPY_OK) {
        ENTROPY_LOG_ERROR_CAT_F("Example",
            "Failed to add group: %s", entropy_status_to_string(status));
        entropy_work_service_destroy(service);
        entropy_work_contract_group_destroy(group);
        return 1;
    }
    ENTROPY_LOG_INFO_CAT_F("Example", "Group registered");

    // 4. Start the service
    ENTROPY_LOG_INFO_CAT_F("Example", "Step 4: Starting work service...");
    entropy_work_service_start(service, &status);
    if (status != ENTROPY_OK) {
        ENTROPY_LOG_ERROR_CAT_F("Example",
            "Failed to start service: %s", entropy_status_to_string(status));
        entropy_work_service_destroy(service);
        entropy_work_contract_group_destroy(group);
        return 1;
    }
    ENTROPY_LOG_INFO_CAT_F("Example", "Service started");

    // 5. Create and schedule background work
    ENTROPY_LOG_INFO_CAT_F("Example", "Step 5: Creating and scheduling 10 background tasks...");
    TaskContext* contexts = malloc(sizeof(TaskContext) * 10);
    entropy_WorkContractHandle* handles = malloc(sizeof(entropy_WorkContractHandle) * 10);

    for (int i = 0; i < 10; i++) {
        contexts[i].task_id = i;
        contexts[i].message = "Processing background data";

        handles[i] = entropy_work_contract_group_create_contract(
            group, example_work, &contexts[i], ENTROPY_EXEC_ANY_THREAD, &status
        );

        if (status != ENTROPY_OK) {
            ENTROPY_LOG_WARNING_CAT_F("Example",
                "Failed to create contract %d: %s", i, entropy_status_to_string(status));
            continue;
        }

        // Schedule the contract
        EntropyScheduleResult result = entropy_work_contract_schedule(handles[i], &status);
        if (result == ENTROPY_SCHEDULE_SCHEDULED) {
            ENTROPY_LOG_DEBUG_CAT_F("Example", "Task %d scheduled", i);
        }
    }

    // 6. Create main thread work
    ENTROPY_LOG_INFO_CAT_F("Example", "Step 6: Creating main thread tasks...");
    TaskContext main_contexts[3];
    entropy_WorkContractHandle main_handles[3];

    for (int i = 0; i < 3; i++) {
        main_contexts[i].task_id = 100 + i;
        main_contexts[i].message = "Updating UI";

        main_handles[i] = entropy_work_contract_group_create_contract(
            group, main_thread_work, &main_contexts[i],
            ENTROPY_EXEC_MAIN_THREAD, &status
        );

        if (status == ENTROPY_OK) {
            entropy_work_contract_schedule(main_handles[i], &status);
            ENTROPY_LOG_DEBUG_CAT_F("Example", "Main thread task %d scheduled", 100 + i);
        }
    }

    // 7. Execute main thread work
    ENTROPY_LOG_INFO_CAT_F("Example", "Step 7: Executing main thread work...");
    if (entropy_work_service_has_main_thread_work(service)) {
        EntropyMainThreadWorkResult result;
        entropy_work_service_execute_main_thread_work(service, 0, &result, &status);
        ENTROPY_LOG_INFO_CAT_F("Example",
            "Executed %zu contracts from %zu groups",
            result.contracts_executed, result.groups_with_work);
    }

    // Wait for initial work to complete before starting re-entrant example
    entropy_work_contract_group_wait(group, &status);
    ENTROPY_LOG_INFO_CAT_F("Example", "Initial work completed");

    // 8. Re-entrant work contracts (tasks spawning tasks)
    ENTROPY_LOG_INFO_CAT_F("Example", "Step 8: Re-entrant work contracts (recursive task spawning)...");
    ENTROPY_LOG_INFO_CAT_F("Example", "Creating binary tree of tasks (depth 3 = 15 total tasks)");

    // Create root task that will spawn children recursively
    ReentrantContext* root = malloc(sizeof(ReentrantContext));
    if (root) {
        root->group = group;
        root->depth = 0;
        root->max_depth = 3;  // Creates 2^3 - 1 = 7 tasks at depth 3, 15 total
        root->task_id = 1;    // Root is task 1

        entropy_WorkContractHandle root_handle = entropy_work_contract_group_create_contract(
            group, reentrant_work, root, ENTROPY_EXEC_ANY_THREAD, &status
        );

        if (status == ENTROPY_OK) {
            entropy_work_contract_schedule(root_handle, &status);
            ENTROPY_LOG_INFO_CAT_F("Example", "Root task scheduled - it will spawn children recursively");
        }

        // Wait for the recursive tree to complete
        ENTROPY_LOG_INFO_CAT_F("Example", "Waiting for re-entrant task tree to complete...");
        entropy_work_contract_group_wait(group, &status);
        ENTROPY_LOG_INFO_CAT_F("Example", "Re-entrant task tree completed");

        // Destroy the root handle
        entropy_work_contract_handle_destroy(root_handle);
    }

    // 9. Print statistics
    ENTROPY_LOG_INFO_CAT_F("Example", "Step 9: Final statistics:");
    ENTROPY_LOG_INFO_CAT_F("Example", "  Capacity: %zu",
        entropy_work_contract_group_capacity(group));
    ENTROPY_LOG_INFO_CAT_F("Example", "  Active contracts: %zu",
        entropy_work_contract_group_active_count(group));
    ENTROPY_LOG_INFO_CAT_F("Example", "  Scheduled contracts: %zu",
        entropy_work_contract_group_scheduled_count(group));
    ENTROPY_LOG_INFO_CAT_F("Example", "  Executing contracts: %zu",
        entropy_work_contract_group_executing_count(group));

    // 10. Cleanup
    ENTROPY_LOG_INFO_CAT_F("Example", "Step 10: Cleaning up...");
    entropy_work_service_stop(service, &status);
    ENTROPY_LOG_DEBUG_CAT_F("Example", "Service stopped");

    // Destroy handle wrappers
    ENTROPY_LOG_DEBUG_CAT_F("Example", "Destroying work contract handles...");
    for (int i = 0; i < 10; i++) {
        entropy_work_contract_handle_destroy(handles[i]);
    }
    for (int i = 0; i < 3; i++) {
        entropy_work_contract_handle_destroy(main_handles[i]);
    }

    entropy_work_service_destroy(service);
    ENTROPY_LOG_DEBUG_CAT_F("Example", "Service destroyed");

    entropy_work_contract_group_destroy(group);
    ENTROPY_LOG_DEBUG_CAT_F("Example", "Group destroyed");

    free(contexts);
    free(handles);
    ENTROPY_LOG_DEBUG_CAT_F("Example", "Memory freed");

    ENTROPY_LOG_INFO_CAT_F("Example", "=== Example completed successfully ===");
    return 0;
}
