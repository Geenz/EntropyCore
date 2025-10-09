/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "WorkContractGroup.h"
#include "../TypeSystem/TypeID.h"
#include <format>
#include "IConcurrencyProvider.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>

namespace EntropyEngine {
namespace Core {
namespace Concurrency {
    static size_t roundUpToPowerOf2(size_t n) {
        if (n <= 1) return 1;
        return static_cast<size_t>(std::pow(2, std::ceil(std::log2(n))));
    }

    // Helper function to create appropriately sized SignalTree
    std::unique_ptr<SignalTreeBase> WorkContractGroup::createSignalTree(size_t capacity) {
        size_t leafCount = (capacity + 63) / 64;
        // Ensure minimum of 2 leaves to avoid single-node tree bug
        // where the same node serves as both root counter and leaf bitmap
        size_t powerOf2 = std::max(roundUpToPowerOf2(leafCount), size_t(2));
        
        return std::make_unique<SignalTree>(powerOf2);
    }

    WorkContractGroup::WorkContractGroup(size_t capacity, std::string name)
        : _capacity(capacity)
        , _contracts(capacity)
        , _name(name) {
        
        // Create SignalTree for ready contracts
        _readyContracts = createSignalTree(capacity);
        
        // Create SignalTree for main thread contracts
        _mainThreadContracts = createSignalTree(capacity);
        
        // Initialize the lock-free free list
        // Build a linked list through all slots
        for (size_t i = 0; i < _capacity - 1; ++i) {
            _contracts[i].nextFree.store(static_cast<uint32_t>(i + 1), std::memory_order_relaxed);
        }
        // Last slot points to INVALID_INDEX
        _contracts[_capacity - 1].nextFree.store(INVALID_INDEX, std::memory_order_relaxed);
        
        // Head points to first slot
        _freeListHead.store(0, std::memory_order_relaxed);
    }
    
    WorkContractGroup::WorkContractGroup(WorkContractGroup&& other) noexcept
        : _capacity(other._capacity)
        , _contracts(std::move(other._contracts))
        , _readyContracts(std::move(other._readyContracts))
        , _mainThreadContracts(std::move(other._mainThreadContracts))
        , _freeListHead(other._freeListHead.load(std::memory_order_acquire))
        , _activeCount(other._activeCount.load(std::memory_order_acquire))
        , _scheduledCount(other._scheduledCount.load(std::memory_order_acquire))
        , _executingCount(other._executingCount.load(std::memory_order_acquire))
        , _selectingCount(other._selectingCount.load(std::memory_order_acquire))
        , _mainThreadScheduledCount(other._mainThreadScheduledCount.load(std::memory_order_acquire))
        , _mainThreadExecutingCount(other._mainThreadExecutingCount.load(std::memory_order_acquire))
        , _mainThreadSelectingCount(other._mainThreadSelectingCount.load(std::memory_order_acquire))
        , _name(std::move(other._name))
        , _concurrencyProvider(other._concurrencyProvider)
        , _stopping(other._stopping.load(std::memory_order_acquire))
    {
        // Clear the other object to prevent double cleanup
        other._concurrencyProvider = nullptr;
        other._stopping.store(true, std::memory_order_release);
        other._activeCount.store(0, std::memory_order_release);
        other._scheduledCount.store(0, std::memory_order_release);
        other._executingCount.store(0, std::memory_order_release);
        other._selectingCount.store(0, std::memory_order_release);
        other._mainThreadScheduledCount.store(0, std::memory_order_release);
        other._mainThreadExecutingCount.store(0, std::memory_order_release);
        other._mainThreadSelectingCount.store(0, std::memory_order_release);
    }
    
    WorkContractGroup& WorkContractGroup::operator=(WorkContractGroup&& other) noexcept {
        if (this != &other) {
            // Clean up current state
            stop();
            wait();
            // Clear the provider reference
            _concurrencyProvider = nullptr;
            
            // Move from other
            const_cast<size_t&>(_capacity) = other._capacity;
            _contracts = std::move(other._contracts);
            _readyContracts = std::move(other._readyContracts);
            _mainThreadContracts = std::move(other._mainThreadContracts);
            _freeListHead.store(other._freeListHead.load(std::memory_order_acquire), std::memory_order_release);
            _activeCount.store(other._activeCount.load(std::memory_order_acquire), std::memory_order_release);
            _scheduledCount.store(other._scheduledCount.load(std::memory_order_acquire), std::memory_order_release);
            _executingCount.store(other._executingCount.load(std::memory_order_acquire), std::memory_order_release);
            _selectingCount.store(other._selectingCount.load(std::memory_order_acquire), std::memory_order_release);
            _mainThreadScheduledCount.store(other._mainThreadScheduledCount.load(std::memory_order_acquire), std::memory_order_release);
            _mainThreadExecutingCount.store(other._mainThreadExecutingCount.load(std::memory_order_acquire), std::memory_order_release);
            _mainThreadSelectingCount.store(other._mainThreadSelectingCount.load(std::memory_order_acquire), std::memory_order_release);
            _name = std::move(other._name);
            _concurrencyProvider = other._concurrencyProvider;
            _stopping.store(other._stopping.load(std::memory_order_acquire), std::memory_order_release);
            
            // Clear the other object
            other._concurrencyProvider = nullptr;
            other._stopping.store(true, std::memory_order_release);
            other._activeCount.store(0, std::memory_order_release);
            other._scheduledCount.store(0, std::memory_order_release);
            other._executingCount.store(0, std::memory_order_release);
            other._mainThreadScheduledCount.store(0, std::memory_order_release);
            other._mainThreadExecutingCount.store(0, std::memory_order_release);
            other._selectingCount.store(0, std::memory_order_release);
            other._mainThreadSelectingCount.store(0, std::memory_order_release);
        }
        return *this;
    }
    
    void WorkContractGroup::releaseAllContracts() {
        // Iterate through all contract slots and release any that are still allocated or scheduled
        for (uint32_t i = 0; i < _capacity; ++i) {
            auto& slot = _contracts[i];
            
            // Check if this slot is occupied (not free)
            ContractState currentState = slot.state.load(std::memory_order_acquire);
            if (currentState != ContractState::Free) {
                // Try to transition directly to Free state
                ContractState expected = currentState;
                if (slot.state.compare_exchange_strong(expected, ContractState::Free,
                                                      std::memory_order_acq_rel)) {
                    // Successfully transitioned, now clean up
                    bool isMainThread = (slot.executionType == ExecutionType::MainThread);
                    returnSlotToFreeList(i, currentState, isMainThread);
                }
                // If CAS failed, another thread (or our own iteration) already handled this slot
                // This is fine - we just continue to the next slot
            }
        }
    }

    void WorkContractGroup::unscheduleAllContracts() {
        // Iterate through all contract slots and unschedule any that are scheduled
        for (uint32_t i = 0; i < _capacity; ++i) {
            auto& slot = _contracts[i];
            
            // Check if this slot is scheduled
            ContractState currentState = slot.state.load(std::memory_order_acquire);
            if (currentState == ContractState::Scheduled) {
                // Try to transition from Scheduled to Allocated
                ContractState expected = ContractState::Scheduled;
                if (slot.state.compare_exchange_strong(expected, ContractState::Allocated,
                                                      std::memory_order_acq_rel)) {
                    // Remove from appropriate ready set based on execution type
                    size_t newScheduledCount;
                    if (slot.executionType == ExecutionType::MainThread) {
                        _mainThreadContracts->clear(i);
                        newScheduledCount = _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    } else {
                        _readyContracts->clear(i);
                        newScheduledCount = _scheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    }
                    
                    // Notify waiters if all scheduled contracts are complete
                    if (newScheduledCount == 0) {
                        std::lock_guard<std::mutex> lock(_waitMutex);
                        _waitCondition.notify_all();
                    }
                }
                // If CAS failed, state changed - likely now executing, which is fine
            }
        }
    }

    WorkContractGroup::~WorkContractGroup() {
        // Stop accepting new work first - this prevents any new selections
        stop();
        
        // Wait for executing work to complete
        // This ensures no thread is in the middle of selectForExecution
        wait();

        // Unschedule all scheduled contracts first (move them back to allocated state)
        // This ensures we don't have contracts stuck in scheduled state
        unscheduleAllContracts();

        // Release all remaining contracts (allocated and any still scheduled)
        // This ensures no contracts are left hanging when the group is destroyed
        releaseAllContracts();

        // Validate that all contracts have been properly cleaned up
        ENTROPY_DEBUG_BLOCK(
            size_t activeCount = _activeCount.load(std::memory_order_acquire);
            ENTROPY_ASSERT(activeCount == 0, "WorkContractGroup destroyed with active contracts still allocated");

            // Double-check that no threads are still selecting
            size_t selectingCount = _selectingCount.load(std::memory_order_acquire);
            ENTROPY_ASSERT(selectingCount == 0, "WorkContractGroup destroyed with threads still in selectForExecution");
            
            size_t mainThreadSelectingCount = _mainThreadSelectingCount.load(std::memory_order_acquire);
            ENTROPY_ASSERT(mainThreadSelectingCount == 0, "WorkContractGroup destroyed with threads still in selectForMainThreadExecution");
        );

        // Then notify the concurrency provider to remove us from active groups
        // CRITICAL: Read provider without holding lock to avoid ABBA deadlock
        IConcurrencyProvider* provider = nullptr;
        {
            std::unique_lock<std::shared_mutex> lock(_concurrencyProviderMutex);
            provider = _concurrencyProvider;
        }

        if (provider) {
            provider->notifyGroupDestroyed(this);
        }
    }

    WorkContractHandle WorkContractGroup::createContract(std::function<void()> work, ExecutionType executionType) {
        // Pop a free slot from the lock-free stack (ABA-resistant with tagged head)
        auto packHead = [](uint32_t idx, uint32_t tag) -> uint64_t {
            return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
        };
        auto headIndex = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h & 0xFFFFFFFFull); };
        auto headTag   = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h >> 32); };

        uint64_t head = _freeListHead.load(std::memory_order_acquire);
        for (;;) {
            uint32_t idx = headIndex(head);
            if (idx == INVALID_INDEX) {
                return WorkContractHandle(); // No free slots available
            }
            uint32_t next = _contracts[idx].nextFree.load(std::memory_order_acquire);
            uint64_t newHead = packHead(next, headTag(head) + 1);
            if (_freeListHead.compare_exchange_weak(head, newHead,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                head = newHead; // Not necessary, but keeps head updated
                // We successfully popped idx
                uint32_t index = idx;
                
                auto& slot = _contracts[index];
                
                // Get current generation for handle before any modifications
                uint32_t generation = slot.generation.load(std::memory_order_acquire);
                
                // Assign work into non-throwing wrapper; avoid exceptions entirely.
                // We wrap the incoming callable to a noexcept thunk.
                bool ok = slot.work.assign([fn = std::move(work)]() noexcept {
                    if (fn) fn();
                });
                if (!ok) {
                    // Allocation failed in wrapper; push slot back to free list and return invalid handle
                    uint64_t old = _freeListHead.load(std::memory_order_acquire);
                    for (;;) {
                        uint32_t oldIdx = headIndex(old);
                        slot.nextFree.store(oldIdx, std::memory_order_release);
                        uint64_t newH = packHead(index, headTag(old) + 1);
                        if (_freeListHead.compare_exchange_weak(old, newH,
                                                                std::memory_order_acq_rel,
                                                                std::memory_order_acquire)) {
                            break;
                        }
                    }
                    return WorkContractHandle();
                }
                slot.executionType = executionType;
                
                // Increment active count BEFORE making the slot visible as allocated.
                // This ensures that any thread that successfully observes the Allocated state
                // (via acquire) also observes the increased activeCount due to release/acquire
                // synchronization on slot.state.
                _activeCount.fetch_add(1, std::memory_order_acq_rel);
                // Transition state to allocated
                slot.state.store(ContractState::Allocated, std::memory_order_release);
                
                return WorkContractHandle(this, static_cast<uint32_t>(index), generation);
            }
            // CAS failed; head updated; retry
        }
    }

    ScheduleResult WorkContractGroup::scheduleContract(const WorkContractHandle& handle) {
        if (!validateHandle(handle)) return ScheduleResult::Invalid;
        
        uint32_t index = handle.handleIndex();
        auto& slot = _contracts[index];
        
        // Try to transition from Allocated to Scheduled
        ContractState expected = ContractState::Allocated;
        if (!slot.state.compare_exchange_strong(expected, ContractState::Scheduled,
                                                std::memory_order_acq_rel)) {
            // Check why it failed
            ContractState current = slot.state.load(std::memory_order_acquire);
            if (current == ContractState::Scheduled) {
                return ScheduleResult::AlreadyScheduled;
            } else if (current == ContractState::Executing) {
                return ScheduleResult::Executing;
            }
            return ScheduleResult::Invalid;
        }
        
        // Add to appropriate ready set based on execution type
        if (slot.executionType == ExecutionType::MainThread) {
            _mainThreadContracts->set(index);
            _mainThreadScheduledCount.fetch_add(1, std::memory_order_acq_rel);
        } else {
            _readyContracts->set(index);
            _scheduledCount.fetch_add(1, std::memory_order_acq_rel);
        }
        
        // Notify concurrency provider if set
        {
            std::shared_lock<std::shared_mutex> lock(_concurrencyProviderMutex);
            if (_concurrencyProvider) {
                _concurrencyProvider->notifyWorkAvailable(this);
            }
        }
        
        return ScheduleResult::Scheduled;
    }

    ScheduleResult WorkContractGroup::unscheduleContract(const WorkContractHandle& handle) {
        // Relaxed validation to preserve semantics under unified execution:
        // If the handle belongs to this group and index is in range, but generation
        // has advanced due to execution starting, report Executing rather than Invalid.
        if (handle.handleOwner() != static_cast<const void*>(this)) {
            return ScheduleResult::Invalid;
        }
        uint32_t index = handle.handleIndex();
        if (index >= _capacity) {
            return ScheduleResult::Invalid;
        }

        auto& slot = _contracts[index];
        uint32_t currentGen = slot.generation.load(std::memory_order_acquire);
        if (currentGen != handle.handleGeneration()) {
            // Slot was freed/reused. It may be due to execution having started (unified flow).
            ContractState st = slot.state.load(std::memory_order_acquire);
            if (st == ContractState::Executing) {
                return ScheduleResult::Executing;
            }
            // In unified flow, we set state to Free while the task is still running.
            if (st == ContractState::Free) {
                size_t exec = _executingCount.load(std::memory_order_acquire) +
                              _mainThreadExecutingCount.load(std::memory_order_acquire);
                if (exec > 0) {
                    return ScheduleResult::Executing;
                }
            }
            return ScheduleResult::Invalid;
        }
        
        // Generation matches: proceed with normal unschedule logic
        // Check current state
        ContractState currentState = slot.state.load(std::memory_order_acquire);
        
        if (currentState == ContractState::Scheduled) {
            // Try to transition back to Allocated
            ContractState expected = ContractState::Scheduled;
            if (slot.state.compare_exchange_strong(expected, ContractState::Allocated,
                                                  std::memory_order_acq_rel)) {
                // Remove from appropriate ready set based on execution type
                size_t newScheduledCount;
                if (slot.executionType == ExecutionType::MainThread) {
                    _mainThreadContracts->clear(index);
                    newScheduledCount = _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
                } else {
                    _readyContracts->clear(index);
                    newScheduledCount = _scheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
                }
                
                // Notify waiters if all scheduled contracts are complete
                if (newScheduledCount == 0) {
                    std::lock_guard<std::mutex> lock(_waitMutex);
                    _waitCondition.notify_all();
                }
                
                return ScheduleResult::NotScheduled;
            }
            // State changed while we were checking - likely now executing
            return ScheduleResult::Executing;
        } else if (currentState == ContractState::Executing) {
            return ScheduleResult::Executing;
        } else if (currentState == ContractState::Allocated) {
            return ScheduleResult::NotScheduled;
        }
        
        return ScheduleResult::Invalid;
    }

    void WorkContractGroup::releaseContract(const WorkContractHandle& handle) {
        if (!validateHandle(handle)) return;

        uint32_t index = handle.handleIndex();
        
        // Bounds check to prevent out-of-bounds access
        if (index >= _capacity) return;
        
        auto& slot = _contracts[index];

        // Atomically try to transition from Allocated or Scheduled to Free.
        // This is the core of handling the race with selectForExecution.
        ContractState currentState = slot.state.load(std::memory_order_acquire);

        while (true) {
            if (currentState == ContractState::Allocated) {
                // Try to transition from Allocated -> Free
                ContractState expected = ContractState::Allocated;
                if (slot.state.compare_exchange_weak(expected, ContractState::Free,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
                    // Success, we are responsible for cleanup
                    bool isMainThread = (slot.executionType == ExecutionType::MainThread);
                    returnSlotToFreeList(index, ContractState::Allocated, isMainThread);
                    return;
                }
                // CAS failed, currentState is updated, loop again
                currentState = expected;
                continue;
            }
            
            if (currentState == ContractState::Scheduled) {
                // Try to transition from Scheduled -> Free
                ContractState expected = ContractState::Scheduled;
                if (slot.state.compare_exchange_weak(expected, ContractState::Free,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
                    // Success, we are responsible for cleanup
                    bool isMainThread = (slot.executionType == ExecutionType::MainThread);
                    returnSlotToFreeList(index, ContractState::Scheduled, isMainThread);
                    return;
                }
                // CAS failed, currentState is updated. It might have become Executing. Loop again.
                currentState = expected;
                continue;
            }
            
            // If we are here, the state is either Free, Executing, or invalid.
            // In any of these cases, this thread can no longer act.
            return;
        }
    }

    bool WorkContractGroup::isValidHandle(const WorkContractHandle& handle) const noexcept {
        return validateHandle(handle);
    }

    WorkContractHandle WorkContractGroup::selectForExecution(std::optional<std::reference_wrapper<uint64_t>> bias) {
        // RAII guard to track threads in selection
        struct SelectionGuard {
            WorkContractGroup* group;
            bool active;
            
            SelectionGuard(WorkContractGroup* g) : group(g), active(true) {
                group->_selectingCount.fetch_add(1, std::memory_order_acq_rel);
            }
            
            ~SelectionGuard() {
                if (active) {
                    auto count = group->_selectingCount.fetch_sub(1, std::memory_order_acq_rel);
                    if (count == 1) {
                        // We were the last selecting thread, notify waiters
                        std::lock_guard<std::mutex> lock(group->_waitMutex);
                        group->_waitCondition.notify_all();
                    }
                }
            }
            
            void deactivate() { active = false; }
        };
        
        SelectionGuard guard(this);
        
        // Don't allow selection if we're stopping
        if (_stopping.load(std::memory_order_seq_cst)) {
            return WorkContractHandle();
        }

        
        // Use provided bias or create a local one
        uint64_t localBias = 0;
        uint64_t& biasRef = bias ? bias->get() : localBias;
        
        // Check stopping flag again right before accessing _readyContracts
        // This reduces the race window significantly
        if (_stopping.load(std::memory_order_seq_cst)) {
            return WorkContractHandle();
        }
        
        auto [index, _] = _readyContracts->select(biasRef);
        
        if (index == SignalTreeBase::S_INVALID_SIGNAL_INDEX) {
            return WorkContractHandle();
        }
        
        auto& slot = _contracts[index];
        
        // Try to transition from Scheduled to Executing
        ContractState expected = ContractState::Scheduled;
        if (!slot.state.compare_exchange_strong(expected, ContractState::Executing,
                                               std::memory_order_acq_rel)) {
            // Someone else got it first or state changed
            return WorkContractHandle();
        }

        // Clear from ready set immediately upon successful selection to avoid stale ready bits.
        // CRITICAL: This clear is part of a triple-redundancy strategy to ensure no stale bits remain
        // in the signal tree under any thread interleaving. See returnSlotToFreeList() for defensive
        // clear that handles the race where this thread is preempted before clearing.
        _readyContracts->clear(index);
        
        // Get current generation for handle
        uint32_t generation = slot.generation.load(std::memory_order_acquire);

        // Update counters: decrement scheduled, increment executing
        _scheduledCount.fetch_sub(1, std::memory_order_acq_rel);
        _executingCount.fetch_add(1, std::memory_order_acq_rel);

        // Return valid handle
        return WorkContractHandle(this, static_cast<uint32_t>(index), generation);
    }
    
    WorkContractHandle WorkContractGroup::selectForMainThreadExecution(std::optional<std::reference_wrapper<uint64_t>> bias) {
        // RAII guard to track threads in selection
        struct SelectionGuard {
            WorkContractGroup* group;
            bool active;
            
            SelectionGuard(WorkContractGroup* g) : group(g), active(true) {
                group->_mainThreadSelectingCount.fetch_add(1, std::memory_order_acq_rel);
            }
            
            ~SelectionGuard() {
                if (active) {
                    auto count = group->_mainThreadSelectingCount.fetch_sub(1, std::memory_order_acq_rel);
                    if (count == 1) {
                        // We were the last selecting thread, notify waiters
                        std::lock_guard<std::mutex> lock(group->_waitMutex);
                        group->_waitCondition.notify_all();
                    }
                }
            }
            
            void deactivate() { active = false; }
        };
        
        SelectionGuard guard(this);
        
        // Don't allow selection if we're stopping
        if (_stopping.load(std::memory_order_seq_cst)) {
            return WorkContractHandle();
        }
        
        // Use provided bias or create a local one
        uint64_t localBias = 0;
        uint64_t& biasRef = bias ? bias->get() : localBias;
        
        // Check stopping flag again right before accessing _mainThreadContracts
        if (_stopping.load(std::memory_order_seq_cst)) {
            return WorkContractHandle();
        }
        
        auto [index, _] = _mainThreadContracts->select(biasRef);
        
        if (index == SignalTreeBase::S_INVALID_SIGNAL_INDEX) {
            return WorkContractHandle();
        }
        
        auto& slot = _contracts[index];
        
        // Try to transition from Scheduled to Executing
        ContractState expected = ContractState::Scheduled;
        if (!slot.state.compare_exchange_strong(expected, ContractState::Executing,
                                               std::memory_order_acq_rel)) {
            // Someone else got it first or state changed
            return WorkContractHandle();
        }

        // Clear from main-thread ready set immediately upon successful selection.
        // CRITICAL: This clear is part of a triple-redundancy strategy to ensure no stale bits remain
        // in the signal tree under any thread interleaving. See returnSlotToFreeList() for defensive
        // clear that handles the race where this thread is preempted before clearing.
        _mainThreadContracts->clear(index);
        
        // Get current generation for handle
        uint32_t generation = slot.generation.load(std::memory_order_acquire);

        // Update counters: decrement scheduled, increment executing
        _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel);
        _mainThreadExecutingCount.fetch_add(1, std::memory_order_acq_rel);

        // Return valid handle
        return WorkContractHandle(this, static_cast<uint32_t>(index), generation);
    }

    void WorkContractGroup::executeContract(const WorkContractHandle& handle) {
        if (!handle.valid()) return;

        const uint32_t index = handle.handleIndex();
        auto& slot = _contracts[index];

        const bool isMainThread = (slot.executionType == ExecutionType::MainThread);

        // Move work out (point of no return)
        auto task = std::move(slot.work);

        // Free the slot BEFORE executing to allow re-entrance
        // Invalidate handles and transition to Free
        slot.generation.fetch_add(1, std::memory_order_acq_rel);
        slot.state.store(ContractState::Free, std::memory_order_release);

        // Layer 3: Defensive clear (guard against selector preemption before clear)
        if (isMainThread) {
            _mainThreadContracts->clear(index);
        } else {
            _readyContracts->clear(index);
        }

        // Return slot to freelist (ABA-resistant)
        // Note: activeCount will be decremented AFTER task execution to maintain
        // the invariant that executing contracts are included in activeCount
        auto packHead = [](uint32_t idx, uint32_t tag) -> uint64_t {
            return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
        };
        auto headIndex = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h & 0xFFFFFFFFull); };
        auto headTag   = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h >> 32); };

        uint64_t old = _freeListHead.load(std::memory_order_acquire);
        for (;;) {
            slot.nextFree.store(headIndex(old), std::memory_order_release);
            uint64_t newH = packHead(index, headTag(old) + 1);
            if (_freeListHead.compare_exchange_weak(old, newH,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                break;
            }
        }


        // Execute outside of slot ownership
        if (task) {
            task();
        }

        // Finally, decrement executing counters and notify if needed
        size_t newExecCount = isMainThread
            ? _mainThreadExecutingCount.fetch_sub(1, std::memory_order_acq_rel) - 1
            : _executingCount.fetch_sub(1, std::memory_order_acq_rel) - 1;

        if (newExecCount == 0) {
            std::lock_guard<std::mutex> lock(_waitMutex);
            _waitCondition.notify_all();
        }

        // Now decrement active count and fire capacity callbacks
        auto newActiveCount = _activeCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (newActiveCount < _capacity) {
            std::lock_guard<std::mutex> lock(_callbackMutex);
            for (const auto& cb : _onCapacityAvailableCallbacks) {
                if (cb) cb();
            }
        }
    }

    void WorkContractGroup::abortExecution(const WorkContractHandle& handle) {
        if (!handle.valid()) return;

        const uint32_t index = handle.handleIndex();
        auto& slot = _contracts[index];

        const bool isMainThread = (slot.executionType == ExecutionType::MainThread);

        // Drop work; we are not executing
        slot.work.reset();

        // Invalidate handles and free the slot
        slot.generation.fetch_add(1, std::memory_order_acq_rel);
        slot.state.store(ContractState::Free, std::memory_order_release);

        // Defensive clear to keep signal tree clean
        if (isMainThread) {
            _mainThreadContracts->clear(index);
        } else {
            _readyContracts->clear(index);
        }

        // Decrement active BEFORE returning to freelist
        _activeCount.fetch_sub(1, std::memory_order_acq_rel);

        // Return slot to freelist (ABA-resistant)
        auto packHead = [](uint32_t idx, uint32_t tag) -> uint64_t {
            return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
        };
        auto headIndex = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h & 0xFFFFFFFFull); };
        auto headTag   = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h >> 32); };

        uint64_t old = _freeListHead.load(std::memory_order_acquire);
        for (;;) {
            slot.nextFree.store(headIndex(old), std::memory_order_release);
            uint64_t newH = packHead(index, headTag(old) + 1);
            if (_freeListHead.compare_exchange_weak(old, newH,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                break;
            }
        }

        // Decrement executing and notify
        size_t newExecCount = isMainThread
            ? _mainThreadExecutingCount.fetch_sub(1, std::memory_order_acq_rel) - 1
            : _executingCount.fetch_sub(1, std::memory_order_acq_rel) - 1;

        if (newExecCount == 0) {
            std::lock_guard<std::mutex> lock(_waitMutex);
            _waitCondition.notify_all();
        }
    }

    void WorkContractGroup::completeExecution(const WorkContractHandle& /*handle*/) {
        // DEPRECATED: No-op for backward compatibility.
        // All cleanup now happens inside executeContract() to enable re-entrance.
        // This method can be safely removed once all call sites are updated.
    }

    void WorkContractGroup::completeMainThreadExecution(const WorkContractHandle& /*handle*/) {
        // DEPRECATED: No-op for backward compatibility.
        // All cleanup now happens inside executeContract() to enable re-entrance.
        // This method can be safely removed once all call sites are updated.
    }
    
    size_t WorkContractGroup::executeAllMainThreadWork() {
        return executeMainThreadWork(std::numeric_limits<size_t>::max());
    }
    
    size_t WorkContractGroup::executeMainThreadWork(size_t maxContracts) {
        size_t executed = 0;
        uint64_t localBias = 0;
        
        while (executed < maxContracts) {
            auto handle = selectForMainThreadExecution(std::ref(localBias));
            if (!handle.valid()) {
                break;  // No more main thread contracts scheduled
            }
            
            // Execute the contract (includes all cleanup)
            executeContract(handle);
            executed++;
            
            // Rotate bias to ensure fairness
            localBias = (localBias << 1) | (localBias >> 63);
        }
        
        return executed;
    }

    void WorkContractGroup::stop() {
        _stopping.store(true, std::memory_order_seq_cst);
        // Wake up any threads waiting in wait()
        _waitCondition.notify_all();
    }
    
    void WorkContractGroup::resume() {
        _stopping.store(false, std::memory_order_seq_cst);
        // Note: We don't notify here - the caller should use their
        // concurrency provider to notify of available work if needed
    }
    
    void WorkContractGroup::wait() {
        // Use condition variable for efficient waiting instead of busy-wait
        std::unique_lock<std::mutex> lock(_waitMutex);
        _waitCondition.wait(lock, [this]() {
            if (_stopping.load(std::memory_order_seq_cst)) {
                // When stopping, wait for both executing work AND selecting threads
                return _executingCount.load(std::memory_order_acquire) == 0 &&
                       _selectingCount.load(std::memory_order_acquire) == 0 &&
                       _mainThreadExecutingCount.load(std::memory_order_acquire) == 0 &&
                       _mainThreadSelectingCount.load(std::memory_order_acquire) == 0;
            }
            // Normal wait - wait for all scheduled AND executing work to complete
            return _scheduledCount.load(std::memory_order_acquire) == 0 && 
                   _executingCount.load(std::memory_order_acquire) == 0 &&
                   _mainThreadScheduledCount.load(std::memory_order_acquire) == 0 &&
                   _mainThreadExecutingCount.load(std::memory_order_acquire) == 0;
        });
    }

    void WorkContractGroup::executeAllBackgroundWork() {
        // Maintain local bias for fair selection
        uint64_t localBias = 0;
        
        // Keep executing until no more scheduled contracts
        while (true) {
            WorkContractHandle handle = selectForExecution(std::ref(localBias));
            if (!handle.valid()) {
                break;  // No more scheduled contracts
            }
            
            // Use the existing executeContract method for consistency (includes all cleanup)
            executeContract(handle);

            // Rotate bias to ensure fairness across all tree branches
            localBias = (localBias << 1) | (localBias >> 63);
        }
    }

    bool WorkContractGroup::validateHandle(const WorkContractHandle& handle) const noexcept {
        // Check owner via stamped identity
        if (handle.handleOwner() != static_cast<const void*>(this)) return false;
        
        // Check index bounds
        uint32_t index = handle.handleIndex();
        if (index >= _capacity) return false;
        
        // Check generation
        uint32_t currentGen = _contracts[index].generation.load(std::memory_order_acquire);
        return currentGen == handle.handleGeneration();
    }
    
    ContractState WorkContractGroup::getContractState(const WorkContractHandle& handle) const noexcept {
        if (!validateHandle(handle)) return ContractState::Free;
        
        uint32_t index = handle.handleIndex();
        return _contracts[index].state.load(std::memory_order_acquire);
    }

    size_t WorkContractGroup::executingCount() const noexcept {
        return _executingCount.load(std::memory_order_acquire);
    }
    
    void WorkContractGroup::returnSlotToFreeList(uint32_t index, ContractState previousState, bool isMainThread) {
        auto& slot = _contracts[index];

        // Increment generation to invalidate all handles
        slot.generation.fetch_add(1, std::memory_order_acq_rel);

        // Clear the work function to release resources
        slot.work.reset();

        // Signal tree clearing strategy (triple-redundancy for correctness):
        // Layer 1: Primary clear immediately after selection (selectForExecution/selectForMainThreadExecution)
        // Layer 2: Scheduled cleanup - clear if released before execution starts
        // Layer 3: Defensive clear - handles race where selection thread was preempted before clearing
        // This ensures no stale ready bits remain in the signal tree regardless of thread scheduling.

        // Layer 2: Clear if contract was released while still scheduled (never selected for execution)
        if (previousState == ContractState::Scheduled) {
            if (isMainThread) {
                _mainThreadContracts->clear(index);
            } else {
                _readyContracts->clear(index);
            }
        }
        
        // Update counters based on previous state
        if (previousState == ContractState::Allocated) {
            // Contract was allocated but never scheduled - only decrement active count
            // (active count will be decremented below)
        } else if (previousState == ContractState::Scheduled) {
            // Only decrement scheduled count if it was scheduled (not yet executing)
            size_t newScheduledCount;
            if (isMainThread) {
                newScheduledCount = _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            } else {
                newScheduledCount = _scheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            }
            
            // Notify waiters if all scheduled contracts are complete
            if (newScheduledCount == 0) {
                std::lock_guard<std::mutex> lock(_waitMutex);
                _waitCondition.notify_all();
            }
        } else if (previousState == ContractState::Executing) {
            // Layer 3: Defensive clear to handle race condition where selectForExecution() successfully
            // transitioned state to Executing but was preempted before executing Layer 1 clear.
            // Edge case scenario:
            //   1. Thread A: select() returns index N, CAS Scheduled->Executing succeeds
            //   2. Thread A: preempted before _readyContracts->clear(N)
            //   3. Thread B: executeContract(N) + completeExecution(N)
            //   4. Without this clear: signal tree still has stale bit N set
            // This defensive clear ensures correctness under all thread interleavings.
            if (isMainThread) {
                _mainThreadContracts->clear(index);
            } else {
                _readyContracts->clear(index);
            }

            size_t newExecutingCount;
            if (isMainThread) {
                newExecutingCount = _mainThreadExecutingCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            } else {
                newExecutingCount = _executingCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            }
            
            // Notify waiters if this was the last executing contract
            // (either when stopping OR when wait() is waiting for all work to complete)
            if (newExecutingCount == 0) {
                std::lock_guard<std::mutex> lock(_waitMutex);
                _waitCondition.notify_all();
            }
        }

        // Always decrement active count BEFORE exposing slot to free list to avoid transient
        // activeCount > capacity windows under contention.
        auto newActiveCount = _activeCount.fetch_sub(1, std::memory_order_acq_rel) - 1;

        // Now push the slot back onto the free list so new createContract() can reuse it (ABA-resistant)
        auto packHead = [](uint32_t idx, uint32_t tag) -> uint64_t {
            return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
        };
        auto headIndex = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h & 0xFFFFFFFFull); };
        auto headTag   = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h >> 32); };

        uint64_t old = _freeListHead.load(std::memory_order_acquire);
        for (;;) {
            uint32_t oldIdx = headIndex(old);
            slot.nextFree.store(oldIdx, std::memory_order_release);
            uint64_t newH = packHead(index, headTag(old) + 1);
            if (_freeListHead.compare_exchange_weak(old, newH,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                break;
            }
        }
        
        // Notify all registered callbacks that capacity is available
        // This allows WorkGraphs to process deferred nodes
        if (newActiveCount < _capacity) {
            std::lock_guard<std::mutex> lock(_callbackMutex);
            for (const auto& callback : _onCapacityAvailableCallbacks) {
                if (callback) {
                    callback();
                }
            }
        }
    }
    
    void WorkContractGroup::setConcurrencyProvider(IConcurrencyProvider* provider) {
        std::unique_lock<std::shared_mutex> lock(_concurrencyProviderMutex);
        _concurrencyProvider = provider;
    }
    
    WorkContractGroup::CapacityCallback WorkContractGroup::addOnCapacityAvailable(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        _onCapacityAvailableCallbacks.push_back(std::move(callback));
        return std::prev(_onCapacityAvailableCallbacks.end());
    }
    
    void WorkContractGroup::removeOnCapacityAvailable(CapacityCallback it) {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        _onCapacityAvailableCallbacks.erase(it);
    }

// Introspection and debug description overrides (EntropyObject)
uint64_t WorkContractGroup::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(EntropyEngine::Core::TypeSystem::createTypeId<WorkContractGroup>().id);
    return hash;
}

std::string WorkContractGroup::toString() const {
    // Include name and capacity for quick identification
    return std::format("{}@{}(name=\"{}\", cap={})",
                       className(), static_cast<const void*>(this), _name, _capacity);
}

std::string WorkContractGroup::debugString() const {
    // Summarize key counters and state. Avoid locks; these are atomics/cold-path reads.
    const auto active = _activeCount.load(std::memory_order_relaxed);
    const auto sched = _scheduledCount.load(std::memory_order_relaxed);
    const auto exec = _executingCount.load(std::memory_order_relaxed);
    const auto sel = _selectingCount.load(std::memory_order_relaxed);
    const auto mainSched = _mainThreadScheduledCount.load(std::memory_order_relaxed);
    const auto mainExec = _mainThreadExecutingCount.load(std::memory_order_relaxed);
    const auto mainSel = _mainThreadSelectingCount.load(std::memory_order_relaxed);
    const bool stopping = _stopping.load(std::memory_order_relaxed);
    const bool hasProvider = (_concurrencyProvider != nullptr);

    return std::format(
        "{} [refs:{} active:{} sched:{} exec:{} sel:{} mainSched:{} mainExec:{} mainSel:{} stopping:{} provider:{}]",
        toString(), refCount(), active, sched, exec, sel, mainSched, mainExec, mainSel, stopping, hasProvider);
}

std::string WorkContractGroup::description() const {
    // For now, same as debugString for richer description
    return debugString();
}

} // namespace Concurrency
} // namespace Core
} // namespace EntropyEngine