/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Engine project.
 */

/**
 * @file WorkService.h
 * @brief Thread pool service with pluggable work scheduling strategies
 *
 * This file contains the WorkService class, which manages a pool of worker threads
 * that execute work from multiple WorkContractGroups. It uses pluggable schedulers
 * to determine work distribution and provides the main execution infrastructure
 * for the concurrency system.
 */

#pragma once
#include <shared_mutex>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <limits>
#include "IWorkScheduler.h"
#include "IConcurrencyProvider.h"
#include "Core/EntropyService.h"

namespace EntropyEngine {
namespace Core {
namespace Concurrency {
class WorkContractGroup;

/**
 * @brief Thread pool service that executes work contracts from multiple groups.
 *
 * Think of WorkService as a team of workers (threads) that grab tasks from different
 * departments (WorkContractGroups). Each department has its own queue of work, and
 * the workers use a pluggable scheduler to decide which department needs help most.
 *
 * The service delegates all scheduling decisions to an IWorkScheduler implementation,
 * allowing you to experiment with different scheduling strategies without modifying
 * the core thread management logic.
 *
 * Perfect for:
 * - Game engines that need to balance rendering, physics, AI, and audio work
 * - Servers that handle different types of requests with varying priorities
 * - Any system with multiple independent work producers that need fair execution
 *
 * Key features:
 * - Lock-free work execution (groups handle their own synchronization)
 * - Pluggable scheduling strategies via IWorkScheduler interface
 * - Thread pool management independent of scheduling logic
 * - No work stealing between groups (intentional for data isolation)
 *
 * @code
 * // Create service with adaptive ranking scheduler (default)
 * WorkService::Config config;
 * config.threadCount = 8;
 * WorkService service(config);
 *
 * // Or use a custom scheduler
 * auto scheduler = std::make_unique<RoundRobinScheduler>(schedulerConfig);
 * WorkService service(config, std::move(scheduler));
 *
 * // Add work groups from different systems
 * service.addWorkContractGroup(&renderingGroup);
 * service.addWorkContractGroup(&physicsGroup);
 * service.addWorkContractGroup(&audioGroup);
 *
 * // Start the workers
 * service.start();
 *
 * // Systems submit work through their groups, service handles distribution
 * // ...
 *
 * // Shutdown when done
 * service.stop();
 * @endcode
 */
class WorkService : public IConcurrencyProvider, public ::EntropyEngine::Core::EntropyService {
    // Shared mutex management of work contract groups
    // HOT PATH (executeWork): shared_lock for concurrent reads
    // COLD PATH (add/remove): unique_lock for exclusive writes
    mutable std::shared_mutex _workContractGroupsMutex;
    std::vector<WorkContractGroup*> _workContractGroups;
    size_t _workContractGroupCount = 0;                               ///< Current count of work contract groups
    std::vector<std::jthread> _threads;                               ///< Worker threads that execute contracts
    std::unique_ptr<IWorkScheduler> _scheduler;                       ///< Scheduler strategy for selecting work groups

    std::atomic<bool> _running = false;

    // Condition variable for efficient waiting when no work is available
    std::condition_variable _workAvailableCV;
    std::mutex _workAvailableMutex;
    std::atomic<bool> _workAvailable{false};

public:
    /**
     * @brief Result structure for main thread work execution
     * 
     * Provides detailed information about the execution results to the caller,
     * allowing them to make informed decisions about scheduling and performance.
     */
    struct MainThreadWorkResult {
        size_t contractsExecuted;    ///< Number of contracts actually executed
        size_t groupsWithWork;       ///< Number of groups that had work available
        bool moreWorkAvailable;      ///< Whether there's more work that could be executed
    };
    
    /**
     * @brief Configuration parameters for the work service.
     *
     * These knobs let you tune the service for your specific workload. The defaults
     * work well for general-purpose work distribution, but you might want to adjust
     * them based on your use case.
     */
    struct Config {
        uint32_t threadCount = 0;                ///< Worker thread count - 0 means use all CPU cores
        size_t maxSoftFailureCount = 5;         ///< Number of times work selection is allowed to fail before sleeping.  Yields after every failure.
        size_t failureSleepTime = 1;             ///< Sleep duration in nanoseconds when no work found - prevents CPU spinning

        // Scheduler-specific configuration
        IWorkScheduler::Config schedulerConfig;   ///< Configuration passed to scheduler
    };

    /**
     * @brief Creates a work service with the specified configuration.
     *
     * The service is created in a stopped state. You must call start() to begin
     * executing work. Thread count is clamped to hardware concurrency, so asking
     * for 1000 threads on an 8-core machine gets you 8 threads.
     *
     * Uses AdaptiveRankingScheduler by default if no scheduler is provided.
     *
     * @param config Service configuration - see Config struct for tuning options
     * @param scheduler Optional custom scheduler (uses AdaptiveRankingScheduler if nullptr)
     *
     * @code
     * // Use default adaptive ranking scheduler
     * WorkService::Config config;
     * config.threadCount = 0;  // Use all CPU cores
     * WorkService service(config);
     *
     * // Or provide a custom scheduler
     * auto scheduler = std::make_unique<RoundRobinScheduler>(schedulerConfig);
     * WorkService service(config, std::move(scheduler));
     * @endcode
     */
    explicit WorkService(Config config, std::unique_ptr<IWorkScheduler> scheduler = nullptr);

    /**
     * @brief Destroys the work service and cleans up all resources.
     *
     * Automatically calls stop() if the service is still running, waits for all
     * threads to finish, then cleans up. Safe to call even if service was never
     * started.
     */
    ~WorkService();

        // EntropyService identity and lifecycle
        const char* id() const override { return "com.entropy.core.work"; }
        const char* name() const override { return "WorkService"; }
        const char* version() const override { return "0.1.0"; }
        std::vector<std::string> dependsOn() const override { return {}; }

        void load() override { /* no-op: configured on construction */ }
        void unload() override { clear(); }

    /**
     * @brief Starts the worker threads and begins executing work.
     *
     * Spawns the configured number of worker threads, each running the adaptive
     * scheduling algorithm. Safe to call multiple times - if already running,
     * does nothing.
     *
     * @code
     * service.start();  // Workers now actively looking for work
     * @endcode
     */
    void start() override;

    /**
     * @brief Signals all worker threads to stop (non-blocking).
     *
     * Requests workers to stop without waiting. Returns immediately while
     * workers complete their current contract before stopping.
     *
     * @code
     * service.requestStop();  // Signal workers to stop (non-blocking)
     * @endcode
     */
    void requestStop();

    /**
     * @brief Waits for all worker threads to finish (blocking).
     *
     * Blocks the calling thread until all worker threads have completed execution.
     * Should be called after requestStop() if you need to ensure all threads
     * have finished before proceeding.
     *
     * @code
     * service.waitForStop();  // Wait for all threads to finish
     * @endcode
     */
    void waitForStop();

    /**
     * @brief Stops all worker threads and waits for them to finish.
     *
     * Convenience method that calls requestStop() followed by waitForStop().
     * This is a blocking call that ensures all threads have completed before
     * returning.
     *
     * @code
     * service.stop();  // Stop and wait for all threads (blocking)
     * @endcode
     */
    void stop() override;

    /**
     * @brief Removes all registered work groups (only when stopped).
     *
     * Nuclear option that unregisters all groups at once. Only works
     * when the service is stopped to prevent race conditions. Mainly useful for
     * testing or complete system resets.
     *
     * @code
     * service.stop();
     * service.clear();  // All groups unregistered
     * // Re-add groups and restart...
     * @endcode
     */
    void clear();

    enum class GroupOperationStatus {
        Added = 0,
        Removed = 1,
        OutOfSpace = 2,
        Exists = 3,
        NotFound = 4
    };

    /**
     * @brief Registers a work group with the service so it can be scheduled.
     *
     * Think of this as adding a new department to your worker pool. Once registered,
     * the group will be included in the ranking algorithm and its work will be
     * executed by the service's threads.
     *
     * Important: This takes a lock and might block briefly. Best practice is to
     * register all your groups during initialization, not during active execution.
     * The service pre-allocates space for maxWorkGroups to avoid resizing.
     *
     * @param contractGroup Pointer to the group to add - must remain valid while registered
     * @return Added if successful, OutOfSpace if at capacity, Exists if already registered
     *
     * @code
     * // Register groups during startup
     * if (service.addWorkContractGroup(&physicsGroup) != GroupOperationStatus::Added) {
     *     LOG_ERROR("Failed to register physics work group");
     * }
     * @endcode
     */
    GroupOperationStatus addWorkContractGroup(WorkContractGroup* contractGroup);

    /**
     * @brief Unregisters a work group from the service.
     *
     * Removes a group from the scheduling rotation. Any work already in the group
     * remains there - this just stops the service from checking it for new work.
     *
     * Important: This takes a lock and might block. Best practice is to remove
     * groups during shutdown or when a system is being disabled, not during active
     * execution.
     *
     * @param contractGroup The group to remove - must be currently registered
     * @return Removed if successful, NotFound if group wasn't registered
     *
     * @code
     * // Clean up during system shutdown
     * service.removeWorkContractGroup(&physicsGroup);
     * @endcode
     */
    GroupOperationStatus removeWorkContractGroup(WorkContractGroup* contractGroup);

    /**
     * @brief Gets the current work contract group count.
     *
     * This will return however many work gcontract groups that are currently registered in the work service.
     * You should use this to check to make sure that there is room to actually register your work contract group,
     * as the work service does not dynamically reallocate its pool of work contract groups.
     *
     * This is intentional to better force right sizing of the group.
     *
     * @code
     * auto currentWorkGroupCount = workService.getWorkContractGroupCount();
     * if (currentWorkGroupCount < workService.getMaxWorkGroups() -1)
     *     workService.addWorkContractGroup(&contractGroup);
     * @endcode
     * @return How many work contract groups that are currently registered in the work service.
     */
    size_t getWorkContractGroupCount() const;

    /**
     * @brief The current thread count.  Ranges from 1 to max concurrency.
     * @return The thread count.
     */
    size_t getThreadCount() const;

    size_t getSoftFailureCount() const;
    size_t setSoftFailureCount(size_t softFailureCount);

    bool isRunning() const;

    /**
     * @brief Gets how long the system will sleep a thread (in nanoseconds).
     * @return How many nanoseconds the system will sleep a thread for after all hard retries have been exhausted.
     */
    size_t getFailureSleepTime() const;

    /**
     * @brief Sets how long the system will sleep a thread (in nanoseconds).
     * @param failureSleepTime How long to sleep a thread for (in nanoseconds).
     * @return How long the system will now sleep a thread for (in nanoseconds).
     */
    size_t setFailureSleepTime(size_t failureSleepTime);

    // IConcurrencyProvider interface implementation
    void notifyWorkAvailable(WorkContractGroup* group = nullptr) override;
    void notifyGroupDestroyed(WorkContractGroup* group) override;

    /**
     * @brief Execute main thread targeted work from all registered groups
     * 
     * Call from your main thread to process UI, rendering, or other main-thread-only
     * work. Distributes execution fairly across groups. Use maxContracts to limit
     * work per frame and maintain responsiveness.
     * 
     * @param maxContracts Maximum number of contracts to execute (default: unlimited)
     * @return MainThreadWorkResult with execution statistics
     * 
     * @code
     * // Game loop with frame budget
     * void gameUpdate() {
     *     // Process up to 10 main thread tasks per frame
     *     auto result = service.executeMainThreadWork(10);
     *     
     *     if (result.moreWorkAvailable) {
     *         // More work pending - will process next frame
     *         needsUpdate = true;
     *     }
     *     
     *     // Continue with rendering
     *     render();
     * }
     * @endcode
     */
    MainThreadWorkResult executeMainThreadWork(size_t maxContracts = std::numeric_limits<size_t>::max());
    
    /**
     * @brief Execute main thread work from a specific group
     * 
     * Use when you need fine-grained control over which group's work executes.
     * Useful for prioritizing certain subsystems over others.
     * 
     * @param group The group to execute work from
     * @param maxContracts Maximum number of contracts to execute
     * @return Number of contracts executed
     * 
     * @code
     * // Prioritize UI work over other main thread tasks
     * size_t uiWork = service.executeMainThreadWork(&uiGroup, 5);
     * size_t otherWork = service.executeMainThreadWork(&miscGroup, 2);
     * @endcode
     */
    size_t executeMainThreadWork(WorkContractGroup* group, size_t maxContracts = std::numeric_limits<size_t>::max());
    
    /**
     * @brief Check if any registered group has main thread work available
     * 
     * Quick non-blocking check to determine if you need to pump main thread work.
     * Use this to avoid unnecessary calls to executeMainThreadWork().
     * 
     * @return true if at least one group has main thread work scheduled
     * 
     * @code
     * // Only pump if there's work to do
     * if (service.hasMainThreadWork()) {
     *     service.executeMainThreadWork(frameWorkBudget);
     * }
     * @endcode
     */
    bool hasMainThreadWork() const;

    /**
     * @brief Reset static thread-local variables (for testing only)
     * @warning This should only be used in test environments to ensure clean state between tests
     */
    static void resetThreadLocalState();

private:
    /**
     * @brief The main execution loop for worker threads - core of the work system
     *
     * This is the heart of the WorkService - each worker thread spends its entire
     * lifetime running this loop. The loop implements a sophisticated work-finding
     * algorithm that balances efficiency with fairness across multiple work groups.
     *
     * The execution flow:
     * 1. Read the current group vector snapshot (lock-free RCU)
     * 2. Ask the scheduler which group to execute from
     * 3. If work found, execute it and notify scheduler of completion
     * 4. If no work found, either sleep or spin based on scheduler advice
     * 5. Handle stop requests and thread-local cleanup
     *
     * Key features:
     * - Lock-free group vector access using epoch-based reclamation
     * - Thread-local failure tracking for adaptive sleep behavior
     * - Scheduler integration for pluggable work distribution strategies
     * - Proper stop token handling for clean shutdown
     * - Exception safety with proper cleanup on thread exit
     *
     * This is private because it's the internal worker thread implementation.
     * Users interact with the system through WorkContractGroup and handles.
     *
     * @param token Stop token for cooperative thread cancellation
     */
    void executeWork(const std::stop_token& token);



    Config _config;

    /// Thread-local failure counter for adaptive sleep behavior
    /// Each worker thread tracks consecutive failures to find work, enabling
    /// gradual backoff from aggressive spinning to efficient sleeping.
    /// Thread-local because each thread should adapt independently to its own workload pattern.
    static thread_local size_t stSoftFailureCount;
    
    /// Thread-local identifier for debugging and scheduler context
    /// Provides a stable thread ID (0 to threadCount-1) for the lifetime of each worker thread.
    /// Thread-local because each thread needs its own unique, persistent identifier.
    static thread_local size_t stThreadId;
};

} // Concurrency
} // Core
} // EntropyEngine

