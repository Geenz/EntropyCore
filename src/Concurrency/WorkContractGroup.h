/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file WorkContractGroup.h
 * @brief Lock-free work contract pool with concurrent scheduling
 * 
 * This file contains the WorkContractGroup class, which manages a pool of work
 * contracts using lock-free data structures. It provides the core scheduling
 * primitives for the concurrency system, enabling work distribution
 * without blocking or contention between threads.
 */

#pragma once

#include "WorkContractHandle.h"
#include "SignalTree.h"
#include "WorkGraphTypes.h"
#include <memory>
#include <vector>
#include <list>
#include <functional>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include "../Core/EntropyObject.h"

namespace EntropyEngine {
namespace Core {
namespace Concurrency {

    // Forward declaration
    class IConcurrencyProvider;

    /**
     * @brief Factory and manager for work contracts with lock-free scheduling
     * 
     * WorkContractGroup implements a work dispatcher capable of managing
     * thousands of tasks without locks or blocking operations. It provides a comprehensive
     * pool of work contracts with allocation, scheduling, and execution primitives suitable
     * for job systems, task graphs, and high-throughput work management scenarios.
     * 
     * The implementation uses SignalTree-based lock-free operations,
     * enabling multiple threads to schedule and select work concurrently without contention.
     * This design is optimized for game engines, parallel processing systems, and applications
     * requiring management of numerous small work units.
     * 
     * Key features:
     * - Lock-free contract scheduling and selection
     * - Generation-based handles prevent use-after-free bugs
     * - Immediate resource cleanup on completion
     * - Statistical monitoring (active/scheduled counts)
     * - Wait functionality for synchronization points
     * 
     * Important: This class provides scheduling primitives without handling parallel
     * execution directly. External executors such as WorkService are required for
     * concurrent work processing. The class functions as a centralized work registry
     * where tasks are posted and claimed by worker threads.
     * 
     * Handle semantics:
     * - WorkContractHandle derives from EntropyObject and is stamped with (owner + index + generation)
     * - Copying a handle copies the stamped identity; validation is performed against this group's slots
     * - The group owns the lifetime; when a slot is released, the object's identity is cleared and the
     *   generation is incremented to invalidate stale handles
     * 
     * @code
     * // Complete workflow: mixed execution with worker service
     * WorkContractGroup group(1024);
     * WorkService service(4);  // 4 worker threads
     * service.addWorkContractGroup(&group);
     * service.start();
     * 
     * // Submit background work
     * std::vector<WorkContractHandle> handles;
     * for (int i = 0; i < 10; ++i) {
     *     auto handle = group.createContract([i]() { 
     *         processData(i); 
     *     });
     *     handle.schedule();
     *     handles.push_back(handle);
     * }
     * 
     * // Submit main thread work
     * auto uiHandle = group.createContract([]() { 
     *     updateProgressBar(); 
     * }, ExecutionType::MainThread);
     * uiHandle.schedule();
     * 
     * // Main thread pumps its work
     * while (group.hasMainThreadWork()) {
     *     group.executeMainThreadWork(5);  // Process up to 5 per frame
     *     renderFrame();
     * }
     * 
     * // Wait for all background work to complete
     * group.wait();
     * service.stop();
     * @endcode
     */
    class WorkContractGroup : public EntropyEngine::Core::EntropyObject {
    public:
        const char* className() const noexcept override { return "WorkContractGroup"; }
        uint64_t classHash() const noexcept override;
        std::string toString() const override;
        std::string debugString() const override;
        std::string description() const override;
    private:
        /// Sentinel value indicating end of lock-free linked list or invalid slot
        /// Used in the free list implementation to mark the end of the chain and
        /// in tagged pointers to indicate null references. Maximum uint32_t value
        /// ensures it's never a valid array index. Static constexpr because it's
        /// a fundamental constant used throughout the lock-free data structure.
        static constexpr uint32_t INVALID_INDEX = ~0u;
        
        
        /**
         * @brief Internal storage for a single work contract
         *
         * Each slot represents one work contract and tracks its lifecycle through
         * atomic state transitions. The generation counter prevents use-after-free
         * by invalidating old handles when slots are reused.
         */
        struct ContractSlot {
            std::atomic<uint32_t> generation{1};           ///< Handle validation counter
            std::atomic<ContractState> state{ContractState::Free}; ///< Current lifecycle state
            std::function<void()> work;                   ///< Work function
            std::atomic<uint32_t> nextFree{INVALID_INDEX}; ///< Next free slot
            ExecutionType executionType{ExecutionType::AnyThread}; ///< Execution context (main/any thread)
        };
        
        std::vector<ContractSlot> _contracts;             ///< Contract storage
        std::unique_ptr<SignalTreeBase> _readyContracts;  ///< Ready work queue
        std::unique_ptr<SignalTreeBase> _mainThreadContracts; ///< Main thread work queue
        std::atomic<uint64_t> _freeListHead{0};           ///< Free list head (packed: [tag:32(upper) | index:32(lower)])

        std::atomic<size_t> _activeCount{0};              ///< Active contract count
        std::atomic<size_t> _scheduledCount{0};           ///< Scheduled count
        std::atomic<size_t> _executingCount{0};           ///< Executing count
        std::atomic<size_t> _selectingCount{0};           ///< Selection in progress
        std::atomic<size_t> _mainThreadScheduledCount{0}; ///< Main thread work pending
        std::atomic<size_t> _mainThreadExecutingCount{0}; ///< Main thread work running
        std::atomic<size_t> _mainThreadSelectingCount{0}; ///< Main thread selection count

        // Synchronization for wait() operations
        mutable std::mutex _waitMutex;                    ///< Mutex for condition variable
        mutable std::condition_variable _waitCondition;   ///< Condition variable for waiting

        std::string _name;
        
        const size_t _capacity;                           ///< Maximum contracts
        
        // Concurrency provider support
        IConcurrencyProvider* _concurrencyProvider = nullptr; ///< Work notification provider
        mutable std::shared_mutex _concurrencyProviderMutex; ///< Protects provider during setup/teardown (COLD PATH ONLY)
        std::list<std::function<void()>> _onCapacityAvailableCallbacks; ///< Capacity callbacks
        mutable std::mutex _callbackMutex; ///< Protects callback list
        
        // Stopping support
        std::atomic<bool> _stopping{false};              ///< Stopping flag

        // Timed deferral support (for WorkGraph timer integration)
        std::function<size_t()> _timedDeferralCallback; ///< Callback for checking timed deferrals

    public:
        
        /**
         * @brief Constructs a work contract group with specified capacity
         * 
         * Pre-allocates all data structures for lock-free operation. Choose capacity
         * based on peak concurrent load.
         * 
         * @param capacity Maximum number of contracts (typically 1024-8192)
         * 
         * @code
         * // For a game engine handling frame tasks
         * WorkContractGroup frameWork(2048);
         * 
         * // For background processing
         * WorkContractGroup backgroundTasks(512);
         * @endcode
         */
        explicit WorkContractGroup(size_t capacity, std::string name = "WorkContractGroup");
        
        /**
         * @brief Destructor ensures all work is stopped and completed
         * 
         * Follows a strict destruction protocol to prevent deadlocks:
         * 1. Calls stop() to prevent new work selection
         * 2. Calls wait() to ensure all executing work completes
         * 3. Unschedules and releases all remaining contracts
         * 4. Reads concurrency provider pointer WITHOUT holding mutex lock
         * 5. Calls notifyGroupDestroyed() to inform provider of destruction
         *
         * CRITICAL: The provider notification is made without holding the group's
         * concurrency provider mutex to prevent ABBA deadlock with WorkService.
         * Any deviation from this protocol may result in deadlock during destruction.
         *
         * The provider will then:
         * - Remove this group from its internal lists
         * - Call setConcurrencyProvider(nullptr) to clear the back-reference
         *
         * This ensures proper bidirectional cleanup without lock ordering issues.
         */
        ~WorkContractGroup();
        
        // Delete copy operations - lock-free data structures shouldn't be copied
        WorkContractGroup(const WorkContractGroup&) = delete;
        WorkContractGroup& operator=(const WorkContractGroup&) = delete;
        
        // Move operations
        WorkContractGroup(WorkContractGroup&& other) noexcept;
        WorkContractGroup& operator=(WorkContractGroup&& other) noexcept;
        
        /**
         * @brief Creates a new work contract with the given work function
         * 
         * @param work Function to execute when contract runs (should be thread-safe)
         * @param executionType Where this contract should be executed (default: AnyThread)
         * @return Handle to the created contract, or invalid handle if group is full
         * 
         * @code
         * // Simple work for any thread
         * auto handle = group.createContract([]() {
         *     std::cout << "Hello from work thread!\n";
         * });
         * 
         * // Main thread targeted work
         * auto mainHandle = group.createContract([]() {
         *     updateUI();
         * }, ExecutionType::MainThread);
         * 
         * // Check if creation succeeded
         * if (!handle.valid()) {
         *     std::cerr << "Group is full - can't create more work\n";
         * }
         * @endcode
         */
        WorkContractHandle createContract(std::function<void()> work, 
                                        ExecutionType executionType = ExecutionType::AnyThread);
        
        /**
         * @brief Waits for all scheduled and executing contracts to complete
         * 
         * Blocks until all work finishes. Includes scheduled and executing contracts.
         * 
         * @code
         * // Submit a batch of work
         * for (int i = 0; i < 100; ++i) {
         *     auto handle = group.createContract([i]() { processItem(i); });
         *     handle.schedule();
         * }
         * 
         * // Wait for all work to complete
         * group.wait();
         * std::cout << "All work finished!\n";
         * @endcode
         */
        void wait();
        
        /**
         * @brief Stops the group from accepting new work selections
         * 
         * Prevents new work selection. Executing work continues.
         * Thread-safe.
         */
        void stop();
        
        /**
         * @brief Resumes the group to allow new work selections
         * 
         * Clears the stopping flag to allow selectForExecution() to return work
         * again. Does NOT automatically notify waiting threads.
         * 
         * Thread-safe.
         */
        void resume();
        
        /**
         * @brief Checks if the group is in the process of stopping
         * 
         * @return true if stop() has been called, false otherwise
         */
        bool isStopping() const noexcept { return _stopping.load(std::memory_order_seq_cst); }
        
        /**
         * @brief Executes all background (non-main-thread) contracts sequentially in the calling thread
         * 
         * Grabs every scheduled background contract and executes them one by one in the current thread.
         * Uses bias rotation to prevent starvation. Does NOT execute main thread targeted contracts.
         * 
         * @code
         * // Schedule several background tasks
         * for (int i = 0; i < 10; ++i) {
         *     auto handle = group.createContract([i]() {
         *         std::cout << "Task " << i << "\n";
         *     }); // Default is ExecutionType::AnyThread
         *     handle.schedule();
         * }
         * 
         * // Execute all background contracts
         * group.executeAllBackgroundWork();
         * // All background tasks are now complete
         * @endcode
         */
        void executeAllBackgroundWork();
        
        /**
         * @brief Gets the maximum capacity of this group
         * 
         * @return Maximum number of contracts this group can handle
         */
        size_t capacity() const noexcept { return _capacity; }

        /**
         * @brief Gets the number of currently allocated contracts
         *
         * @return Number of contracts that have been created but not yet released
         *
         * @code
         * std::cout << "Using " << group.activeCount() << " of "
         *           << group.capacity() << " available slots\n";
         * @endcode
         */
        size_t activeCount() const noexcept { return _activeCount.load(std::memory_order_acquire); }
        
        /**
         * @brief Gets the number of contracts currently scheduled for execution
         * 
         * @return Number of contracts currently scheduled and waiting for execution
         * 
         * @code
         * if (group.scheduledCount() > 100) {
         *     std::cout << "Work load is getting full - might want to throttle\n";
         * }
         * @endcode
         */
        size_t scheduledCount() const noexcept { return _scheduledCount.load(std::memory_order_acquire); }
        
        /**
         * @brief Gets the number of main thread contracts currently scheduled
         * 
         * @return Number of main thread contracts waiting for execution
         */
        size_t mainThreadScheduledCount() const noexcept { 
            return _mainThreadScheduledCount.load(std::memory_order_acquire); 
        }
        
        /**
         * @brief Gets the number of main thread contracts currently executing
         * 
         * @return Number of main thread contracts being executed
         */
        size_t mainThreadExecutingCount() const noexcept { 
            return _mainThreadExecutingCount.load(std::memory_order_acquire); 
        }
        
        /**
         * @brief Checks if there are any main thread contracts ready to execute
         * 
         * @return true if main thread work is available
         */
        bool hasMainThreadWork() const noexcept {
            return mainThreadScheduledCount() > 0;
        }
        
        /**
         * @brief Schedules a contract for execution (called by handle.schedule())
         * 
         * Transitions a contract from Allocated to Scheduled state. Use the handle
         * method instead of calling this directly.
         * 
         * @param handle Handle to the contract to schedule
         * @return Result indicating success or failure reason
         */
        ScheduleResult scheduleContract(const WorkContractHandle& handle);
        
        /**
         * @brief Removes a contract from scheduling (called by handle.unschedule())
         * 
         * Removes from ready list if not yet executing. Use handle method instead.
         * 
         * @param handle Handle to the contract to unschedule
         * @return Result indicating success or failure reason
         */
        ScheduleResult unscheduleContract(const WorkContractHandle& handle);
        
        /**
         * @brief Immediately releases a contract (called by handle.release())
         * 
         * Forcibly frees a contract. Use the handle method instead.
         * 
         * @param handle Handle to the contract to release
         */
        void releaseContract(const WorkContractHandle& handle);
        
        /**
         * @brief Validates a handle belongs to this group (called by handle.valid())
         * 
         * Checks handle validity and generation. Use handle method instead.
         * 
         * @param handle Handle to validate
         * @return true if handle is valid and belongs to this group
         */
        bool isValidHandle(const WorkContractHandle& handle) const noexcept;
        
        /**
         * @brief Selects a scheduled contract for execution
         * 
         * Atomically transitions a contract from Scheduled to Executing state.
         * 
         * @param bias Optional selection bias for fair work distribution
         * @return Handle to an executing contract, or invalid handle if none available
         */
        WorkContractHandle selectForExecution(std::optional<std::reference_wrapper<uint64_t>> bias = std::nullopt);
        
        /**
         * @brief Selects a main thread scheduled contract for execution
         * 
         * Use this from your main thread to pick up work that must run there.
         * Typically called in a loop until no more work is available. Thread-safe
         * with other selections.
         * 
         * @param bias Optional selection bias for fair work distribution
         * @return Handle to an executing contract, or invalid handle if none available
         * 
         * @code
         * // Main thread pump pattern
         * uint64_t bias = 0;
         * while (auto handle = group.selectForMainThreadExecution(std::ref(bias))) {
         *     group.executeContract(handle);
         *     group.completeMainThreadExecution(handle);
         * }
         * @endcode
         */
        WorkContractHandle selectForMainThreadExecution(std::optional<std::reference_wrapper<uint64_t>> bias = std::nullopt);
        
        /**
         * @brief Executes all main thread targeted work contracts
         * 
         * Convenience method that handles the full pump cycle internally.
         * Use this when you want to drain all main thread work at once.
         * Must be called from the main thread.
         * 
         * @return Number of contracts actually executed
         * 
         * @code
         * // In your game loop or UI thread
         * void updateMainThread() {
         *     size_t executed = group.executeAllMainThreadWork();
         *     if (executed > 0) {
         *         LOG_DEBUG("Processed {} main thread tasks", executed);
         *     }
         * }
         * @endcode
         */
        size_t executeAllMainThreadWork();
        
        /**
         * @brief Executes main thread targeted work contracts with a limit
         * 
         * Use when you need to bound main thread work per frame/iteration.
         * Prevents blocking the main thread for too long. Must be called
         * from the main thread.
         * 
         * @param maxContracts Maximum number of contracts to execute
         * @return Number of contracts actually executed
         * 
         * @code
         * // Limit main thread work to maintain 60 FPS
         * void gameLoop() {
         *     // Execute at most 5 tasks per frame
         *     size_t executed = group.executeMainThreadWork(5);
         *     renderFrame();
         * }
         * @endcode
         */
        size_t executeMainThreadWork(size_t maxContracts);
        
        /**
         * @brief Executes the work function of a contract
         * 
         * Only call on contracts returned by selectForExecution().
         * 
         * @param handle Handle to the contract to execute (must be in Executing state)
         */
        void executeContract(const WorkContractHandle& handle);
        
        /**
         * @brief Aborts execution without running the task (shutdown-only path)
         */
        void abortExecution(const WorkContractHandle& handle);
        
        /**
         * @brief Completes execution and cleans up a contract
         * 
         * Must be called after executeContract() to complete the lifecycle.
         * 
         * @param handle Handle to the contract that finished executing
         */
        void completeExecution(const WorkContractHandle& handle);
        
        /**
         * @brief Completes execution and cleans up a main thread contract
         * 
         * Like completeExecution() but for main thread contracts. Updates the
         * correct counters and frees the contract for reuse. Always call this
         * after executeContract() for main thread work.
         * 
         * @param handle Handle to the main thread contract that finished executing
         * 
         * @code
         * auto handle = group.selectForMainThreadExecution();
         * if (handle.valid()) {
         *     group.executeContract(handle);
         *     group.completeMainThreadExecution(handle);  // Essential cleanup
         * }
         * @endcode
         */
        void completeMainThreadExecution(const WorkContractHandle& handle);
        
        /**
         * @brief Gets the current state of a contract
         * 
         * @param handle Handle to query
         * @return Current state of the contract, or Free if handle is invalid
         */
        ContractState getContractState(const WorkContractHandle& handle) const noexcept;

        /**
         * @brief Returns the current number of contracts being actively executed
         *
         * Useful for thread scheduling and load balancing decisions.
         *
         * @return The number of currently executing contracts
         */
        size_t executingCount() const noexcept;
        
        /**
         * @brief Associates this group with a concurrency provider
         * 
         * Provider will be notified when work becomes available. Call during
         * setup/teardown, not during active work execution.
         * 
         * @param provider The concurrency provider to associate with, or nullptr to clear
         */
        void setConcurrencyProvider(IConcurrencyProvider* provider);
        
        /**
         * @brief Gets the currently associated concurrency provider
         * 
         * @return The current provider, or nullptr if none is set
         */
        IConcurrencyProvider* getConcurrencyProvider() const noexcept { return _concurrencyProvider; }
        
        using CapacityCallback = std::list<std::function<void()>>::iterator;
        
        /**
         * @brief Add a callback to be invoked when capacity becomes available
         * 
         * Called after a contract completes and frees up capacity.
         * 
         * @param callback Function to call when capacity is available
         * @return Iterator that can be used to remove the callback
         */
        CapacityCallback addOnCapacityAvailable(std::function<void()> callback);
        
        /**
         * @brief Remove a capacity available callback
         *
         * @param it Iterator returned from addOnCapacityAvailable
         */
        void removeOnCapacityAvailable(CapacityCallback it);

        /**
         * @brief Checks for timed deferrals and schedules ready nodes
         *
         * Invokes the timed deferral callback if one is set (used by WorkGraph for timer support).
         * Returns 0 if no callback is registered (standard WorkContractGroups don't support timers).
         *
         * @return Number of nodes that were scheduled from timed deferral queue
         */
        size_t checkTimedDeferrals() {
            if (_timedDeferralCallback) {
                return _timedDeferralCallback();
            }
            return 0;
        }

        /**
         * @brief Sets a callback for checking timed deferrals
         *
         * Allows external owners (like WorkGraph) to provide timer functionality
         * without requiring inheritance or RTTI/dynamic_cast.
         *
         * @param callback Function that checks and schedules timed deferrals, or nullptr to clear
         */
        void setTimedDeferralCallback(std::function<size_t()> callback) {
            _timedDeferralCallback = std::move(callback);
        }

    private:
        /**
         * @brief Creates a SignalTree sized appropriately for the given capacity
         * 
         * Handles power-of-2 rounding required by SignalTree's binary structure.
         * 
         * @param capacity Number of work contracts the tree needs to support
         * @return Unique pointer to properly sized SignalTree
         */
        static std::unique_ptr<SignalTreeBase> createSignalTree(size_t capacity);
        
        /**
         * @brief Validates that a handle belongs to this group with correct generation
         * 
         * Internal validation checking owner, bounds, and generation.
         * 
         * @param handle Handle to validate
         * @return true if handle is completely valid for this group
         */
        bool validateHandle(const WorkContractHandle& handle) const noexcept;
        
        /**
         * @brief Returns a contract slot to the free list after cleanup
         * 
         * Increments generation, clears work function, updates counters,
         * and notifies waiters.
         * 
         * @param index The slot index to return to the free list
         * @param previousState The state the slot was in before being freed
         * @param isMainThread Whether this is a main thread contract (default: false)
         */
        void returnSlotToFreeList(uint32_t index, ContractState previousState, bool isMainThread = false);
        
        /**
         * @brief Releases all remaining contracts in the group
         * 
         * Used during destruction to ensure no contracts are left hanging.
         */
        void releaseAllContracts();
        
        /**
         * @brief Unschedules all scheduled contracts in the group
         * 
         * Moves scheduled contracts back to allocated state during destruction.
         */
        void unscheduleAllContracts();
    };

} // namespace Concurrency
} // namespace Core
} // namespace EntropyEngine


