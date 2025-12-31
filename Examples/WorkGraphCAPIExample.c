/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file WorkGraphCAPIExample.c
 * @brief Complete example demonstrating the WorkGraph C API
 *
 * This example shows:
 * - Creating a work graph with dependencies
 * - Adding regular and yieldable nodes
 * - Defining execution dependencies (fan-out, fan-in)
 * - Main thread vs background thread execution
 * - Monitoring progress
 * - Error handling
 * - Yieldable tasks that can suspend and resume
 */

#include <Logging/CLogger.h>
#include <entropy/entropy_work_contract_group.h>
#include <entropy/entropy_work_graph.h>
#include <entropy/entropy_work_service.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Example Context Data
// ============================================================================

typedef struct
{
    int task_id;
    const char* task_name;
    int sleep_ms;
} TaskContext;

typedef struct
{
    int poll_count;
    int max_polls;
} YieldableContext;

// ============================================================================
// Work Functions
// ============================================================================

void load_data_task(void* user_data) {
    TaskContext* ctx = (TaskContext*)user_data;
    ENTROPY_LOG_INFO_CAT_F("LoadData", "Task %d: Loading data...", ctx->task_id);
    // Simulate work
    for (volatile int i = 0; i < 1000000; i++);
    ENTROPY_LOG_INFO_CAT_F("LoadData", "Task %d: Data loaded successfully", ctx->task_id);
}

void process_data_task(void* user_data) {
    TaskContext* ctx = (TaskContext*)user_data;
    ENTROPY_LOG_INFO_CAT_F("ProcessData", "Task %d: Processing %s...", ctx->task_id, ctx->task_name);
    // Simulate work
    for (volatile int i = 0; i < 2000000; i++);
    ENTROPY_LOG_INFO_CAT_F("ProcessData", "Task %d: Processing complete", ctx->task_id);
}

void merge_results_task(void* user_data) {
    TaskContext* ctx = (TaskContext*)user_data;
    ENTROPY_LOG_INFO_CAT_F("MergeResults", "Merging all processed results...");
    // Simulate work
    for (volatile int i = 0; i < 1500000; i++);
    ENTROPY_LOG_INFO_CAT_F("MergeResults", "Results merged successfully");
}

void update_ui_task(void* user_data) {
    TaskContext* ctx = (TaskContext*)user_data;
    ENTROPY_LOG_INFO_CAT_F("UI", "[MAIN THREAD] Updating UI with final results...");
    // This runs on the main thread - safe for UI operations
    ENTROPY_LOG_INFO_CAT_F("UI", "[MAIN THREAD] UI updated successfully");
}

// Yieldable task that polls until a condition is met
EntropyWorkResult polling_task(void* user_data) {
    YieldableContext* ctx = (YieldableContext*)user_data;

    ctx->poll_count++;
    ENTROPY_LOG_DEBUG_CAT_F("Poller", "Poll attempt %d/%d", ctx->poll_count, ctx->max_polls);

    if (ctx->poll_count < ctx->max_polls) {
        // Not ready yet - yield and try again later
        return ENTROPY_WORK_YIELD;
    }

    // Condition met - complete the task
    ENTROPY_LOG_INFO_CAT_F("Poller", "Polling complete after %d attempts", ctx->poll_count);
    return ENTROPY_WORK_COMPLETE;
}

// ============================================================================
// Helper Functions
// ============================================================================

void check_status(const char* operation, EntropyStatus status) {
    if (status != ENTROPY_OK) {
        ENTROPY_LOG_ERROR_CAT_F("Error", "%s failed: %s", operation, entropy_status_to_string(status));
        exit(1);
    }
}

void print_stats(entropy_WorkGraph graph) {
    EntropyStatus status = ENTROPY_OK;
    EntropyWorkGraphStats stats;
    entropy_work_graph_get_stats(graph, &stats, &status);

    if (status == ENTROPY_OK) {
        ENTROPY_LOG_INFO_CAT_F("Stats", "Total: %u | Completed: %u | Failed: %u | Pending: %u | Executing: %u",
                               stats.total_nodes, stats.completed_nodes, stats.failed_nodes, stats.pending_nodes,
                               stats.executing_nodes);
    }
}

// ============================================================================
// Main Example
// ============================================================================

int main(void) {
    EntropyStatus status = ENTROPY_OK;

    ENTROPY_LOG_INFO_F("======================================");
    ENTROPY_LOG_INFO_F("WorkGraph C API Example");
    ENTROPY_LOG_INFO_F("======================================");

    // ========================================================================
    // Step 1: Create Work Contract Group
    // ========================================================================

    ENTROPY_LOG_INFO_CAT_F("Setup", "Creating work contract group...");
    entropy_WorkContractGroup group = entropy_work_contract_group_create(2048, "WorkGraphExample", &status);
    check_status("Create work contract group", status);

    // ========================================================================
    // Step 2: Create Work Service (Thread Pool)
    // ========================================================================

    ENTROPY_LOG_INFO_CAT_F("Setup", "Creating work service with 4 threads...");
    EntropyWorkServiceConfig service_config;
    entropy_work_service_config_init(&service_config);
    service_config.thread_count = 4;  // 4 worker threads

    entropy_WorkService service = entropy_work_service_create(&service_config, &status);
    check_status("Create work service", status);

    entropy_work_service_add_group(service, group, &status);
    check_status("Add group to service", status);

    entropy_work_service_start(service, &status);
    check_status("Start work service", status);

    // ========================================================================
    // Step 3: Create Work Graph
    // ========================================================================

    ENTROPY_LOG_INFO_CAT_F("Setup", "Creating work graph...");
    EntropyWorkGraphConfig graph_config;
    entropy_work_graph_config_init(&graph_config);
    graph_config.expected_node_count = 10;

    entropy_WorkGraph graph = entropy_work_graph_create_with_config(group, &graph_config, &status);
    check_status("Create work graph", status);

    // ========================================================================
    // Step 4: Build the Task Graph
    // ========================================================================

    ENTROPY_LOG_INFO_CAT_F("Setup", "Building task dependency graph...");

    // Root task: Load data
    TaskContext* load_ctx = malloc(sizeof(TaskContext));
    load_ctx->task_id = 1;
    load_ctx->task_name = "loader";

    entropy_NodeHandle load_node =
        entropy_work_graph_add_node(graph, load_data_task, load_ctx, "LoadData", ENTROPY_EXEC_ANY_THREAD, &status);
    check_status("Add load node", status);

    // Fan-out: Three parallel processing tasks
    TaskContext* process_ctx1 = malloc(sizeof(TaskContext));
    process_ctx1->task_id = 2;
    process_ctx1->task_name = "chunk-1";

    TaskContext* process_ctx2 = malloc(sizeof(TaskContext));
    process_ctx2->task_id = 3;
    process_ctx2->task_name = "chunk-2";

    TaskContext* process_ctx3 = malloc(sizeof(TaskContext));
    process_ctx3->task_id = 4;
    process_ctx3->task_name = "chunk-3";

    entropy_NodeHandle process1 = entropy_work_graph_add_node(graph, process_data_task, process_ctx1, "Process-1",
                                                              ENTROPY_EXEC_ANY_THREAD, &status);
    check_status("Add process1 node", status);

    entropy_NodeHandle process2 = entropy_work_graph_add_node(graph, process_data_task, process_ctx2, "Process-2",
                                                              ENTROPY_EXEC_ANY_THREAD, &status);
    check_status("Add process2 node", status);

    entropy_NodeHandle process3 = entropy_work_graph_add_node(graph, process_data_task, process_ctx3, "Process-3",
                                                              ENTROPY_EXEC_ANY_THREAD, &status);
    check_status("Add process3 node", status);

    // Yieldable polling task (runs in parallel with processing)
    YieldableContext* yield_ctx = malloc(sizeof(YieldableContext));
    yield_ctx->poll_count = 0;
    yield_ctx->max_polls = 5;

    entropy_NodeHandle poller = entropy_work_graph_add_yieldable_node(
        graph, polling_task, yield_ctx, "Poller", ENTROPY_EXEC_ANY_THREAD, 10, &status  // Max 10 reschedules
    );
    check_status("Add yieldable node", status);

    // Fan-in: Merge results after all processing completes
    TaskContext* merge_ctx = malloc(sizeof(TaskContext));
    merge_ctx->task_id = 5;
    merge_ctx->task_name = "merger";

    entropy_NodeHandle merge = entropy_work_graph_add_node(graph, merge_results_task, merge_ctx, "MergeResults",
                                                           ENTROPY_EXEC_ANY_THREAD, &status);
    check_status("Add merge node", status);

    // Main thread UI update (depends on merge completing)
    TaskContext* ui_ctx = malloc(sizeof(TaskContext));
    ui_ctx->task_id = 6;
    ui_ctx->task_name = "ui-updater";

    entropy_NodeHandle ui_update = entropy_work_graph_add_node(
        graph, update_ui_task, ui_ctx, "UpdateUI", ENTROPY_EXEC_MAIN_THREAD, &status  // Must run on main thread
    );
    check_status("Add UI node", status);

    // ========================================================================
    // Step 5: Define Dependencies
    // ========================================================================

    ENTROPY_LOG_INFO_CAT_F("Setup", "Defining task dependencies...");

    // Load → Process1, Process2, Process3 (fan-out)
    entropy_work_graph_add_dependency(graph, load_node, process1, &status);
    check_status("Add load→process1 dependency", status);

    entropy_work_graph_add_dependency(graph, load_node, process2, &status);
    check_status("Add load→process2 dependency", status);

    entropy_work_graph_add_dependency(graph, load_node, process3, &status);
    check_status("Add load→process3 dependency", status);

    // Process1,2,3 → Merge (fan-in)
    entropy_work_graph_add_dependency(graph, process1, merge, &status);
    check_status("Add process1→merge dependency", status);

    entropy_work_graph_add_dependency(graph, process2, merge, &status);
    check_status("Add process2→merge dependency", status);

    entropy_work_graph_add_dependency(graph, process3, merge, &status);
    check_status("Add process3→merge dependency", status);

    // Merge → UI Update (main thread)
    entropy_work_graph_add_dependency(graph, merge, ui_update, &status);
    check_status("Add merge→ui dependency", status);

    // Poller runs independently (no dependencies)

    ENTROPY_LOG_INFO_CAT_F("Setup", "Task graph built successfully");
    ENTROPY_LOG_INFO_CAT_F("Setup", "  - 1 load task");
    ENTROPY_LOG_INFO_CAT_F("Setup", "  - 3 parallel processing tasks");
    ENTROPY_LOG_INFO_CAT_F("Setup", "  - 1 yieldable polling task");
    ENTROPY_LOG_INFO_CAT_F("Setup", "  - 1 merge task");
    ENTROPY_LOG_INFO_CAT_F("Setup", "  - 1 main thread UI update");

    // ========================================================================
    // Step 6: Execute the Graph
    // ========================================================================

    ENTROPY_LOG_INFO_F("======================================");
    ENTROPY_LOG_INFO_F("Starting Execution");
    ENTROPY_LOG_INFO_F("======================================");

    entropy_work_graph_execute(graph, &status);
    check_status("Execute graph", status);

    // ========================================================================
    // Step 7: Monitor Progress (with Main Thread Work Pump)
    // ========================================================================

    ENTROPY_LOG_INFO_CAT_F("Monitor", "Monitoring progress...");

    int iteration = 0;
    while (!entropy_work_graph_is_complete(graph)) {
        // Execute main thread work (UI updates, etc.)
        size_t executed =
            entropy_work_contract_group_execute_main_thread_work(group, 5, &status  // Process up to 5 main thread tasks
            );

        if (executed > 0) {
            ENTROPY_LOG_DEBUG_CAT_F("Monitor", "Executed %zu main thread tasks", executed);
        }

        // Print stats periodically
        if (iteration % 10000 == 0) {
            print_stats(graph);
        }

        iteration++;

        // Small yield to prevent busy waiting
        for (volatile int i = 0; i < 10000; i++);
    }

    ENTROPY_LOG_INFO_CAT_F("Monitor", "All tasks completed!");

    // ========================================================================
    // Step 8: Wait and Get Final Results
    // ========================================================================

    ENTROPY_LOG_INFO_F("======================================");
    ENTROPY_LOG_INFO_F("Waiting for Final Results");
    ENTROPY_LOG_INFO_F("======================================");

    EntropyWaitResult result;
    entropy_work_graph_wait(graph, &result, &status);
    check_status("Wait for graph", status);

    // ========================================================================
    // Step 9: Print Final Statistics
    // ========================================================================

    ENTROPY_LOG_INFO_F("======================================");
    ENTROPY_LOG_INFO_F("Execution Summary");
    ENTROPY_LOG_INFO_F("======================================");

    if (result.all_completed) {
        ENTROPY_LOG_INFO_CAT_F("Results", "SUCCESS: All tasks completed");
    } else {
        ENTROPY_LOG_WARNING_CAT_F("Results", "Some tasks did not complete successfully");
    }

    ENTROPY_LOG_INFO_CAT_F("Results", "Completed: %u", result.completed_count);
    ENTROPY_LOG_INFO_CAT_F("Results", "Failed:    %u", result.failed_count);
    ENTROPY_LOG_INFO_CAT_F("Results", "Dropped:   %u", result.dropped_count);

    print_stats(graph);

    // ========================================================================
    // Step 10: Cleanup
    // ========================================================================

    ENTROPY_LOG_INFO_CAT_F("Cleanup", "Stopping work service...");
    entropy_work_service_stop(service, &status);
    check_status("Stop work service", status);

    ENTROPY_LOG_INFO_CAT_F("Cleanup", "Destroying graph and resources...");

    // Destroy node handles
    entropy_node_handle_destroy(load_node);
    entropy_node_handle_destroy(process1);
    entropy_node_handle_destroy(process2);
    entropy_node_handle_destroy(process3);
    entropy_node_handle_destroy(poller);
    entropy_node_handle_destroy(merge);
    entropy_node_handle_destroy(ui_update);

    // Destroy graph
    entropy_work_graph_destroy(graph);

    // Destroy service and group
    entropy_work_service_destroy(service);
    entropy_work_contract_group_destroy(group);

    // Free contexts
    free(load_ctx);
    free(process_ctx1);
    free(process_ctx2);
    free(process_ctx3);
    free(yield_ctx);
    free(merge_ctx);
    free(ui_ctx);

    ENTROPY_LOG_INFO_F("======================================");
    ENTROPY_LOG_INFO_F("Example Complete");
    ENTROPY_LOG_INFO_F("======================================");

    return 0;
}
