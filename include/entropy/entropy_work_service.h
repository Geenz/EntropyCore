/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file entropy_work_service.h
 * @brief C API for work service (thread pool)
 *
 * A work service is a thread pool that executes contracts from one or more
 * work contract groups using a pluggable scheduling strategy. It manages
 * worker threads and distributes work fairly across registered groups.
 *
 * Perfect for:
 * - Game engines that need to balance rendering, physics, AI, and audio work
 * - Servers that handle different types of requests with varying priorities
 * - Any system with multiple independent work producers that need fair execution
 *
 * Key features:
 * - Lock-free work execution (groups handle their own synchronization)
 * - Adaptive scheduling strategies
 * - Thread pool management
 * - Main thread work separation
 *
 * Thread Safety: All functions in this file are thread-safe unless
 * otherwise documented.
 */

#pragma once

#include "entropy_concurrency_types.h"
#include "entropy_work_contract_group.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// WorkService Lifecycle
// ============================================================================

/**
 * @brief Creates a work service with the specified configuration
 *
 * The service is created in a stopped state. You must call
 * entropy_work_service_start() to begin executing work. Thread count is
 * clamped to hardware concurrency.
 *
 * Uses AdaptiveRankingScheduler internally for intelligent work distribution.
 *
 * @param config Service configuration (required) - see EntropyWorkServiceConfig
 * @param status Output parameter for error reporting (required)
 * @return Owned pointer (must call entropy_work_service_destroy) or NULL on error
 *
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * EntropyWorkServiceConfig config;
 * entropy_work_service_config_init(&config);
 * config.thread_count = 0;  // Use all CPU cores
 *
 * entropy_WorkService service = entropy_work_service_create(&config, &status);
 * if (status != ENTROPY_OK) {
 *     fprintf(stderr, "Failed to create service: %s\n",
 *             entropy_status_to_string(status));
 *     return -1;
 * }
 * @endcode
 */
ENTROPY_API entropy_WorkService entropy_work_service_create(const EntropyWorkServiceConfig* config,
                                                            EntropyStatus* status);

/**
 * @brief Destroys a work service and cleans up all resources
 *
 * Automatically calls stop() if the service is still running, waits for all
 * threads to finish, then cleans up. Safe to call even if service was never
 * started. Safe to call with NULL (no-op).
 *
 * @param service The work service to destroy (can be NULL)
 *
 * @threadsafety Thread-safe
 * @ownership Frees the service
 *
 * @code
 * // Automatic cleanup
 * entropy_work_service_destroy(service);
 * @endcode
 */
ENTROPY_API void entropy_work_service_destroy(entropy_WorkService service);

// ============================================================================
// Service Control
// ============================================================================

/**
 * @brief Starts the worker threads and begins executing work
 *
 * Spawns the configured number of worker threads, each running the adaptive
 * scheduling algorithm. Safe to call multiple times - if already running,
 * returns ENTROPY_ERR_ALREADY_RUNNING.
 *
 * @param service The work service (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * entropy_work_service_start(service, &status);
 * if (status == ENTROPY_OK) {
 *     printf("Workers now actively looking for work\n");
 * }
 * @endcode
 */
ENTROPY_API void entropy_work_service_start(entropy_WorkService service, EntropyStatus* status);

/**
 * @brief Signals all worker threads to stop (non-blocking)
 *
 * Requests workers to stop without waiting. Returns immediately while
 * workers complete their current contract before stopping.
 *
 * @param service The work service (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * entropy_work_service_request_stop(service, &status);  // Non-blocking
 * @endcode
 */
ENTROPY_API void entropy_work_service_request_stop(entropy_WorkService service, EntropyStatus* status);

/**
 * @brief Waits for all worker threads to finish (blocking)
 *
 * Blocks the calling thread until all worker threads have completed execution.
 * Should be called after request_stop() if you need to ensure all threads
 * have finished before proceeding.
 *
 * @param service The work service (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * entropy_work_service_request_stop(service, &status);
 * entropy_work_service_wait_for_stop(service, &status);  // Wait for completion
 * @endcode
 */
ENTROPY_API void entropy_work_service_wait_for_stop(entropy_WorkService service, EntropyStatus* status);

/**
 * @brief Stops all worker threads and waits for them to finish
 *
 * Convenience method that calls request_stop() followed by wait_for_stop().
 * This is a blocking call that ensures all threads have completed before
 * returning.
 *
 * @param service The work service (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * entropy_work_service_stop(service, &status);  // Stop and wait (blocking)
 * @endcode
 */
ENTROPY_API void entropy_work_service_stop(entropy_WorkService service, EntropyStatus* status);

/**
 * @brief Checks if the service is currently running
 *
 * @param service The work service (required)
 * @return ENTROPY_TRUE if running, ENTROPY_FALSE otherwise
 *
 * @threadsafety Thread-safe
 *
 * @code
 * if (entropy_work_service_is_running(service)) {
 *     printf("Service is active\n");
 * }
 * @endcode
 */
ENTROPY_API EntropyBool entropy_work_service_is_running(entropy_WorkService service);

// ============================================================================
// Group Management
// ============================================================================

/**
 * @brief Registers a work group with the service
 *
 * Once registered, the group will be included in the scheduling algorithm
 * and its work will be executed by the service's threads.
 *
 * IMPORTANT: This is a COLD PATH operation. Best practice is to register
 * all your groups during initialization, not during active execution.
 *
 * @param service The work service (required)
 * @param group Pointer to the group to add (required, must remain valid while registered)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe (but may block briefly)
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * entropy_work_service_add_group(service, physics_group, &status);
 * if (status != ENTROPY_OK) {
 *     fprintf(stderr, "Failed to register group: %s\n",
 *             entropy_status_to_string(status));
 * }
 * @endcode
 */
ENTROPY_API void entropy_work_service_add_group(entropy_WorkService service, entropy_WorkContractGroup group,
                                                EntropyStatus* status);

/**
 * @brief Unregisters a work group from the service
 *
 * Removes a group from the scheduling rotation. Any work already in the group
 * remains there - this just stops the service from checking it for new work.
 *
 * IMPORTANT: This is a COLD PATH operation. Best practice is to remove
 * groups during shutdown or when a system is being disabled.
 *
 * @param service The work service (required)
 * @param group The group to remove (required, must be currently registered)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe (but may block briefly)
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * entropy_work_service_remove_group(service, physics_group, &status);
 * @endcode
 */
ENTROPY_API void entropy_work_service_remove_group(entropy_WorkService service, entropy_WorkContractGroup group,
                                                   EntropyStatus* status);

/**
 * @brief Removes all registered work groups (only when stopped)
 *
 * Nuclear option that unregisters all groups at once. Only works when the
 * service is stopped to prevent race conditions. Mainly useful for testing
 * or complete system resets.
 *
 * @param service The work service (required)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety Thread-safe (but service must be stopped)
 *
 * @code
 * EntropyStatus status = ENTROPY_OK;
 * entropy_work_service_stop(service, &status);
 * entropy_work_service_clear(service, &status);
 * // Re-add groups and restart...
 * @endcode
 */
ENTROPY_API void entropy_work_service_clear(entropy_WorkService service, EntropyStatus* status);

// ============================================================================
// Service Statistics
// ============================================================================

/**
 * @brief Gets the current work contract group count
 *
 * Returns the number of groups currently registered with the service.
 *
 * @param service The work service (required)
 * @return Number of registered groups
 *
 * @threadsafety Thread-safe
 *
 * @code
 * size_t count = entropy_work_service_get_group_count(service);
 * printf("Managing %zu work groups\n", count);
 * @endcode
 */
ENTROPY_API size_t entropy_work_service_get_group_count(entropy_WorkService service);

/**
 * @brief Gets the current thread count
 *
 * Returns the number of worker threads. Ranges from 1 to hardware concurrency.
 *
 * @param service The work service (required)
 * @return The thread count
 *
 * @threadsafety Thread-safe
 *
 * @code
 * size_t threads = entropy_work_service_get_thread_count(service);
 * printf("Running with %zu worker threads\n", threads);
 * @endcode
 */
ENTROPY_API size_t entropy_work_service_get_thread_count(entropy_WorkService service);

// ============================================================================
// Main Thread Work Execution
// ============================================================================

/**
 * @brief Execute main thread targeted work from all registered groups
 *
 * Call from your main thread to process UI, rendering, or other main-thread-only
 * work. Distributes execution fairly across groups. Use max_contracts to limit
 * work per frame and maintain responsiveness.
 *
 * @param service The work service (required)
 * @param max_contracts Maximum number of contracts to execute (0 = unlimited)
 * @param result Output parameter for execution statistics (can be NULL)
 * @param status Output parameter for error reporting (required)
 *
 * @threadsafety NOT thread-safe - must be called from main thread only
 *
 * @code
 * // Game loop with frame budget
 * void game_update() {
 *     EntropyStatus status = ENTROPY_OK;
 *     EntropyMainThreadWorkResult result;
 *
 *     // Process up to 10 main thread tasks per frame
 *     entropy_work_service_execute_main_thread_work(
 *         service, 10, &result, &status
 *     );
 *
 *     if (result.more_work_available) {
 *         // More work pending - will process next frame
 *         needs_update = 1;
 *     }
 *
 *     render_frame();
 * }
 * @endcode
 */
ENTROPY_API void entropy_work_service_execute_main_thread_work(entropy_WorkService service, size_t max_contracts,
                                                               EntropyMainThreadWorkResult* result,
                                                               EntropyStatus* status);

/**
 * @brief Execute main thread work from a specific group
 *
 * Use when you need fine-grained control over which group's work executes.
 * Useful for prioritizing certain subsystems over others.
 *
 * @param service The work service (required)
 * @param group The specific group to execute work from (required)
 * @param max_contracts Maximum number of contracts to execute (0 = unlimited)
 * @param status Output parameter for error reporting (required)
 * @return Number of contracts executed
 *
 * @threadsafety NOT thread-safe - must be called from main thread only
 *
 * @code
 * // Prioritize UI work over other main thread tasks
 * EntropyStatus status = ENTROPY_OK;
 * size_t ui_work = entropy_work_service_execute_main_thread_work_from_group(
 *     service, ui_group, 5, &status
 * );
 * size_t other_work = entropy_work_service_execute_main_thread_work_from_group(
 *     service, misc_group, 2, &status
 * );
 * @endcode
 */
ENTROPY_API size_t entropy_work_service_execute_main_thread_work_from_group(entropy_WorkService service,
                                                                            entropy_WorkContractGroup group,
                                                                            size_t max_contracts,
                                                                            EntropyStatus* status);

/**
 * @brief Check if any registered group has main thread work available
 *
 * Quick non-blocking check to determine if you need to pump main thread work.
 * Use this to avoid unnecessary calls to execute_main_thread_work().
 *
 * @param service The work service (required)
 * @return ENTROPY_TRUE if at least one group has main thread work scheduled
 *
 * @threadsafety Thread-safe
 *
 * @code
 * // Only pump if there's work to do
 * if (entropy_work_service_has_main_thread_work(service)) {
 *     EntropyStatus status = ENTROPY_OK;
 *     entropy_work_service_execute_main_thread_work(
 *         service, frame_work_budget, NULL, &status
 *     );
 * }
 * @endcode
 */
ENTROPY_API EntropyBool entropy_work_service_has_main_thread_work(entropy_WorkService service);

#ifdef __cplusplus
}
#endif
