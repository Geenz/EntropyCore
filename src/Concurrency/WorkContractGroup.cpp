/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "WorkContractGroup.h"

#include <chrono>
#include <cmath>
#include <format>
#include <iostream>
#include <limits>

#include <tracy/Tracy.hpp>  // main-thread work-queue profiling (per-frame "gap")

#include "../Debug/CpuZoneProfiler.h"  // headless `cpu.zones` mirror

#include "../Logging/Logger.h"
#include "../TypeSystem/TypeID.h"
#include "IConcurrencyProvider.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{
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

WorkContractGroup::WorkContractGroup(size_t capacity, std::string name, size_t maxPinnedLanes)
    : _contracts(capacity), _name(std::move(name)), _capacity(capacity) {
    // Create SignalTree for ready contracts
    _readyContracts = createSignalTree(capacity);

    // Create SignalTree for main thread contracts
    _mainThreadContracts = createSignalTree(capacity);

    // Pinned lanes: fixed at construction so selection can address them lock-free
    _pinnedLanes.reserve(maxPinnedLanes);
    for (size_t i = 0; i < maxPinnedLanes; ++i) {
        _pinnedLanes.push_back(createSignalTree(capacity));
    }

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

void WorkContractGroup::releaseAllContracts() {
    // Iterate through all contract slots and release any that are still allocated or scheduled
    for (uint32_t i = 0; i < _capacity; ++i) {
        auto& slot = _contracts[i];

        // Check if this slot is occupied (not free)
        ContractState currentState = slot.state.load(std::memory_order_acquire);
        if (currentState != ContractState::Free) {
            // Try to transition directly to Free state
            ContractState expected = currentState;
            if (slot.state.compare_exchange_strong(expected, ContractState::Free, std::memory_order_acq_rel)) {
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
            if (slot.state.compare_exchange_strong(expected, ContractState::Allocated, std::memory_order_acq_rel)) {
                // Remove from the slot's own ready queue
                bool isMainThread = (slot.executionType == ExecutionType::MainThread);
                readyTreeFor(slot).clear(i);

                // Decrement under _waitMutex and notify while holding it so a
                // waiter can only observe the zero once we are done with the group.
                {
                    std::lock_guard<std::mutex> lock(_waitMutex);
                    size_t newScheduledCount =
                        isMainThread ? _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1
                                     : _scheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    if (newScheduledCount == 0) {
                        _waitCondition.notify_all();
                    }
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
        ENTROPY_ASSERT(mainThreadSelectingCount == 0,
                       "WorkContractGroup destroyed with threads still in selectForMainThreadExecution"););

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

WorkContractHandle WorkContractGroup::createContract(std::function<void()> work, ExecutionType executionType,
                                                     uint32_t pinnedLane) {
    // A pin to a lane nothing pumps is a wait() hang wearing a disguise; refuse
    // it at the door.
    if (executionType == ExecutionType::PinnedThread && pinnedLane >= _pinnedLanes.size()) {
        ENTROPY_LOG_ERROR_CAT("WorkContractGroup",
                              std::format("createContract: pinned lane {} out of range (maxPinnedLanes={})", pinnedLane,
                                          _pinnedLanes.size()));
        return WorkContractHandle();
    }

    // Pop a free slot from the lock-free stack (ABA-resistant with tagged head)
    auto packHead = [](uint32_t idx, uint32_t tag) -> uint64_t {
        return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
    };
    auto headIndex = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h & 0xFFFFFFFFull); };
    auto headTag = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h >> 32); };

    uint64_t head = _freeListHead.load(std::memory_order_acquire);
    for (;;) {
        uint32_t idx = headIndex(head);
        if (idx == INVALID_INDEX) {
            return WorkContractHandle();  // No free slots available
        }
        uint32_t next = _contracts[idx].nextFree.load(std::memory_order_acquire);
        uint64_t newHead = packHead(next, headTag(head) + 1);
        if (_freeListHead.compare_exchange_weak(head, newHead, std::memory_order_acq_rel, std::memory_order_acquire)) {
            // We successfully popped idx
            uint32_t index = idx;

            auto& slot = _contracts[index];

            // Get current generation for handle before any modifications
            uint32_t generation = slot.generation.load(std::memory_order_acquire);

            // Assign work with noexcept wrapper to ensure termination on exceptions
            slot.work = [fn = std::move(work)]() noexcept {
                if (fn) fn();
            };
            slot.executionType = executionType;
            slot.pinnedLane = (executionType == ExecutionType::PinnedThread) ? pinnedLane : INVALID_INDEX;

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
    if (!slot.state.compare_exchange_strong(expected, ContractState::Scheduled, std::memory_order_acq_rel)) {
        // Check why it failed
        ContractState current = slot.state.load(std::memory_order_acquire);
        if (current == ContractState::Scheduled) {
            return ScheduleResult::AlreadyScheduled;
        } else if (current == ContractState::Executing) {
            return ScheduleResult::Executing;
        }
        return ScheduleResult::Invalid;
    }

    // Re-validate the generation now that we own the transition: between the
    // entry validation and the CAS the slot may have been freed and reallocated
    // (same Allocated state, different contract). The recycler bumps the
    // generation before the slot can be reused, so a matching generation proves
    // the claimed contract is ours; on mismatch, undo the claim. (Same pattern
    // as releaseContract; the new owner's own schedule() can spuriously observe
    // AlreadyScheduled during the nanosecond revert window, which is benign.)
    if (slot.generation.load(std::memory_order_acquire) != handle.handleGeneration()) {
        slot.state.store(ContractState::Allocated, std::memory_order_release);
        return ScheduleResult::Invalid;
    }

    // Add to appropriate ready set based on execution type.
    // Count BEFORE bit: a worker can select the instant the bit is visible, and
    // its decrement must never be able to precede this increment (the counter is
    // unsigned; a sub-before-add transiently reads as SIZE_MAX and the sub-side's
    // ==0 notify check never fires, stranding wait()).
    // Pinned work counts with background work; its lane tree answers per-lane
    // questions, and wait()'s predicate stays a two-class conjunction.
    if (slot.executionType == ExecutionType::MainThread) {
        _mainThreadScheduledCount.fetch_add(1, std::memory_order_acq_rel);
        _mainThreadContracts->set(index);
    } else {
        _scheduledCount.fetch_add(1, std::memory_order_acq_rel);
        readyTreeFor(slot).set(index);
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
        if (slot.state.compare_exchange_strong(expected, ContractState::Allocated, std::memory_order_acq_rel)) {
            // Re-validate the generation now that we own the transition (same
            // pattern as releaseContract/scheduleContract): a mismatch means we
            // just un-scheduled a freshly-scheduled unrelated contract. Undo
            // the claim and re-assert its queue bit in case its rightful
            // selector consumed the bit and gave up against our transient
            // Allocated state.
            if (slot.generation.load(std::memory_order_acquire) != handle.handleGeneration()) {
                slot.state.store(ContractState::Scheduled, std::memory_order_release);
                readyTreeFor(slot).set(index);
                return ScheduleResult::Invalid;
            }

            // Remove from the slot's own ready queue
            bool isMainThread = (slot.executionType == ExecutionType::MainThread);
            readyTreeFor(slot).clear(index);

            // Decrement under _waitMutex and notify while holding it: this
            // decrement can make wait()'s predicate true, so the waiter must not
            // be able to observe it before we are done touching the group.
            {
                std::lock_guard<std::mutex> lock(_waitMutex);
                size_t newScheduledCount = isMainThread
                                               ? _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1
                                               : _scheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (newScheduledCount == 0) {
                    _waitCondition.notify_all();
                }
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
            if (slot.state.compare_exchange_weak(expected, ContractState::Free, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                // Re-validate the generation now that we own the transition. Between
                // the entry validation and this CAS the slot may have been freed and
                // reallocated (same state, new contract). The recycler bumps the
                // generation before the slot can be reused, so a matching generation
                // proves the claimed contract is ours; on mismatch, undo the claim.
                // (The new owner's schedule() can spuriously fail during this
                // nanosecond revert window; it reports Invalid, which is the same
                // result a fully-released handle already produces.)
                if (slot.generation.load(std::memory_order_acquire) != handle.handleGeneration()) {
                    slot.state.store(ContractState::Allocated, std::memory_order_release);
                    return;
                }
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
            if (slot.state.compare_exchange_weak(expected, ContractState::Free, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                // Same post-CAS generation re-validation as the Allocated path above.
                // Also re-assert the new contract's queue bit: a selector may have
                // consumed it and given up against our transient Free state, and a
                // scheduled contract with no bit is stranded.
                if (slot.generation.load(std::memory_order_acquire) != handle.handleGeneration()) {
                    slot.state.store(ContractState::Scheduled, std::memory_order_release);
                    readyTreeFor(slot).set(index);
                    return;
                }
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
    struct SelectionGuard
    {
        WorkContractGroup* group;
        bool active;

        SelectionGuard(WorkContractGroup* g) : group(g), active(true) {
            group->_selectingCount.fetch_add(1, std::memory_order_acq_rel);
        }

        ~SelectionGuard() {
            if (active) {
                // Decrement under _waitMutex and notify while still holding it.
                // wait()'s predicate reads this counter under the same mutex, so a
                // waiter (including the destructor) can only observe zero after we
                // are completely done touching the group. A bare fetch_sub lets a
                // spuriously-woken waiter see zero, return, and destroy the group
                // while this destructor still holds a pointer to it.
                std::lock_guard<std::mutex> lock(group->_waitMutex);
                if (group->_selectingCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    group->_waitCondition.notify_all();
                }
            }
        }

        void deactivate() {
            active = false;
        }
    };

    SelectionGuard guard(this);

    // Don't allow selection if we're stopping
    if (_stopDepth.load(std::memory_order_seq_cst) > 0) {
        return WorkContractHandle();
    }

    // Use provided bias or create a local one
    uint64_t localBias = 0;
    uint64_t& biasRef = bias ? bias->get() : localBias;

    // Check stopping flag again right before accessing _readyContracts
    // This reduces the race window significantly
    if (_stopDepth.load(std::memory_order_seq_cst) > 0) {
        return WorkContractHandle();
    }

    return claimBackgroundContract(*_readyContracts, ExecutionType::AnyThread, INVALID_INDEX, biasRef);
}

WorkContractHandle WorkContractGroup::claimBackgroundContract(SignalTreeBase& tree, ExecutionType expectedType,
                                                              uint32_t expectedLane, uint64_t& bias) {
    auto [index, _] = tree.select(bias);

    if (index == SignalTreeBase::S_INVALID_SIGNAL_INDEX) {
        return WorkContractHandle();
    }

    auto& slot = _contracts[index];

    // Try to transition from Scheduled to Executing
    ContractState expected = ContractState::Scheduled;
    if (!slot.state.compare_exchange_strong(expected, ContractState::Executing, std::memory_order_acq_rel)) {
        // Someone else got it first or state changed
        return WorkContractHandle();
    }

    // Re-check queue membership now that we own the slot. The bit we consumed
    // may be stale from a previous occupant (freed and reallocated between the
    // tree select and our CAS), and the slot may now belong to a different
    // queue (main thread, another pinned lane, or the shared queue). Undo the
    // claim and re-assert the slot's OWN queue bit: that queue's rightful
    // selector may have consumed its bit and given up against our transient
    // Executing state, and a scheduled contract with no bit is stranded (an
    // extra stale bit is tolerated by design; a missing one is not). A
    // releaseContract racing the transient Executing state no-ops,
    // indistinguishable from racing a genuine execution start.
    if (slot.executionType != expectedType ||
        (expectedType == ExecutionType::PinnedThread && slot.pinnedLane != expectedLane)) {
        slot.state.store(ContractState::Scheduled, std::memory_order_release);
        readyTreeFor(slot).set(index);
        return WorkContractHandle();
    }

    // Clear from this queue immediately upon successful selection to avoid stale ready bits.
    // CRITICAL: This clear is part of a triple-redundancy strategy to ensure no stale bits remain
    // in the signal tree under any thread interleaving. See returnSlotToFreeList() for defensive
    // clear that handles the race where this thread is preempted before clearing.
    tree.clear(index);

    // Get current generation for handle
    uint32_t generation = slot.generation.load(std::memory_order_acquire);

    // Update counters: increment executing BEFORE decrementing scheduled, so
    // wait()'s conjunction (scheduled==0 && executing==0) can never observe the
    // claimed contract in neither counter mid-handoff and return early.
    // Pinned work shares the background counters.
    _executingCount.fetch_add(1, std::memory_order_acq_rel);
    _scheduledCount.fetch_sub(1, std::memory_order_acq_rel);

    // Return valid handle
    return WorkContractHandle(this, static_cast<uint32_t>(index), generation);
}

WorkContractHandle WorkContractGroup::selectForPinnedExecution(size_t lane,
                                                               std::optional<std::reference_wrapper<uint64_t>> bias) {
    if (lane >= _pinnedLanes.size()) {
        return WorkContractHandle();
    }

    // Same guard/counter class as background selection: pinned claims register
    // in _selectingCount/_executingCount, which the destruction protocol waits on.
    struct SelectionGuard
    {
        WorkContractGroup* group;

        explicit SelectionGuard(WorkContractGroup* g) : group(g) {
            group->_selectingCount.fetch_add(1, std::memory_order_acq_rel);
        }

        ~SelectionGuard() {
            // Decrement under _waitMutex and notify while holding it; see the
            // background SelectionGuard for the rationale.
            std::lock_guard<std::mutex> lock(group->_waitMutex);
            if (group->_selectingCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                group->_waitCondition.notify_all();
            }
        }
    };

    SelectionGuard guard(this);

    if (_stopDepth.load(std::memory_order_seq_cst) > 0) {
        return WorkContractHandle();
    }

    uint64_t localBias = 0;
    uint64_t& biasRef = bias ? bias->get() : localBias;

    if (_stopDepth.load(std::memory_order_seq_cst) > 0) {
        return WorkContractHandle();
    }

    return claimBackgroundContract(*_pinnedLanes[lane], ExecutionType::PinnedThread, static_cast<uint32_t>(lane),
                                   biasRef);
}

size_t WorkContractGroup::executePinnedWork(size_t lane, size_t maxContracts) {
    size_t executed = 0;
    uint64_t localBias = 0;

    while (executed < maxContracts) {
        auto handle = selectForPinnedExecution(lane, std::ref(localBias));
        if (!handle.valid()) {
            break;  // No more contracts scheduled on this lane
        }

        executeContract(handle);
        executed++;

        // Rotate bias to ensure fairness
        localBias = (localBias << 1) | (localBias >> 63);
    }

    return executed;
}

bool WorkContractGroup::hasPinnedWork(size_t lane) const noexcept {
    return lane < _pinnedLanes.size() && !_pinnedLanes[lane]->isEmpty();
}

WorkContractHandle WorkContractGroup::selectForMainThreadExecution(
    std::optional<std::reference_wrapper<uint64_t>> bias) {
    // RAII guard to track threads in selection
    struct SelectionGuard
    {
        WorkContractGroup* group;
        bool active;

        SelectionGuard(WorkContractGroup* g) : group(g), active(true) {
            group->_mainThreadSelectingCount.fetch_add(1, std::memory_order_acq_rel);
        }

        ~SelectionGuard() {
            if (active) {
                // Decrement under _waitMutex and notify while holding it; see the
                // background SelectionGuard for the rationale.
                std::lock_guard<std::mutex> lock(group->_waitMutex);
                if (group->_mainThreadSelectingCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    group->_waitCondition.notify_all();
                }
            }
        }

        void deactivate() {
            active = false;
        }
    };

    SelectionGuard guard(this);

    // Don't allow selection if we're stopping
    if (_stopDepth.load(std::memory_order_seq_cst) > 0) {
        return WorkContractHandle();
    }

    // Use provided bias or create a local one
    uint64_t localBias = 0;
    uint64_t& biasRef = bias ? bias->get() : localBias;

    // Check stopping flag again right before accessing _mainThreadContracts
    if (_stopDepth.load(std::memory_order_seq_cst) > 0) {
        return WorkContractHandle();
    }

    auto [index, _] = _mainThreadContracts->select(biasRef);

    if (index == SignalTreeBase::S_INVALID_SIGNAL_INDEX) {
        return WorkContractHandle();
    }

    auto& slot = _contracts[index];

    // Try to transition from Scheduled to Executing
    ContractState expected = ContractState::Scheduled;
    if (!slot.state.compare_exchange_strong(expected, ContractState::Executing, std::memory_order_acq_rel)) {
        // Someone else got it first or state changed
        return WorkContractHandle();
    }

    // Re-check the execution type now that we own the slot; the main-thread bit
    // we consumed may be stale and the slot reallocated to a background or
    // pinned contract. Undo the claim and re-assert the slot's own queue bit in
    // case its rightful selector consumed it and gave up against our transient
    // state.
    if (slot.executionType != ExecutionType::MainThread) {
        slot.state.store(ContractState::Scheduled, std::memory_order_release);
        readyTreeFor(slot).set(index);
        return WorkContractHandle();
    }

    // Clear from main-thread ready set immediately upon successful selection.
    // CRITICAL: This clear is part of a triple-redundancy strategy to ensure no stale bits remain
    // in the signal tree under any thread interleaving. See returnSlotToFreeList() for defensive
    // clear that handles the race where this thread is preempted before clearing.
    _mainThreadContracts->clear(index);

    // Get current generation for handle
    uint32_t generation = slot.generation.load(std::memory_order_acquire);

    // Update counters: increment executing BEFORE decrementing scheduled so
    // wait() can never observe the claimed contract in neither counter.
    _mainThreadExecutingCount.fetch_add(1, std::memory_order_acq_rel);
    _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel);

    // Return valid handle
    return WorkContractHandle(this, static_cast<uint32_t>(index), generation);
}

void WorkContractGroup::executeContract(const WorkContractHandle& handle) {
    // Covers EVERY contract execution — background worker threads AND the
    // main-thread queue — so Tracy shows the whole concurrency system across all
    // threads (the main-thread ones nest under WorkService::executeMainThreadWork).
    // CpuZoneScope mirrors it into `state get cpu.zones` (gated; self-inflates a
    // little when enabled since this is per-contract — read it as a coarse total).
    ZoneScopedN("WCG::executeContract");
    ::EntropyEngine::Core::Debug::CpuZoneScope _cpuz("WCG::executeContract");
    if (!handle.valid()) return;

    const uint32_t index = handle.handleIndex();
    auto& slot = _contracts[index];

    const bool isMainThread = (slot.executionType == ExecutionType::MainThread);

    // Move work out (point of no return)
    auto task = std::move(slot.work);

    // Layer 3: Defensive clear (guard against selector preemption before clear).
    // Read the slot's queue BEFORE freeing: the fields stay valid until reuse,
    // but the intent is clearer this way.
    auto& ownQueue = readyTreeFor(slot);

    // Free the slot BEFORE executing to allow re-entrance
    // Invalidate handles and transition to Free
    slot.generation.fetch_add(1, std::memory_order_acq_rel);
    slot.state.store(ContractState::Free, std::memory_order_release);

    ownQueue.clear(index);

    // Return slot to freelist (ABA-resistant)
    // Note: activeCount will be decremented AFTER task execution to maintain
    // the invariant that executing contracts are included in activeCount
    auto packHead = [](uint32_t idx, uint32_t tag) -> uint64_t {
        return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
    };
    auto headIndex = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h & 0xFFFFFFFFull); };
    auto headTag = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h >> 32); };

    uint64_t old = _freeListHead.load(std::memory_order_acquire);
    for (;;) {
        slot.nextFree.store(headIndex(old), std::memory_order_release);
        uint64_t newH = packHead(index, headTag(old) + 1);
        if (_freeListHead.compare_exchange_weak(old, newH, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    // Execute outside of slot ownership
    if (task) {
        task();
    }

    // Decrement active count and fire capacity callbacks BEFORE the executing
    // decrement: once executing reaches zero a waiter (including the destructor)
    // may proceed, so everything below the locked decrement would race destruction.
    auto newActiveCount = _activeCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (newActiveCount < _capacity) {
        std::lock_guard<std::mutex> lock(_callbackMutex);
        for (const auto& cb : _onCapacityAvailableCallbacks) {
            if (cb) cb();
        }
    }

    // Final group access: decrement executing under _waitMutex and notify while
    // still holding it, so wait()'s predicate can only observe zero once this
    // thread is completely done with the group.
    {
        std::lock_guard<std::mutex> lock(_waitMutex);
        size_t newExecCount = isMainThread ? _mainThreadExecutingCount.fetch_sub(1, std::memory_order_acq_rel) - 1
                                           : _executingCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (newExecCount == 0) {
            _waitCondition.notify_all();
        }
    }
}

void WorkContractGroup::abortExecution(const WorkContractHandle& handle) {
    if (!handle.valid()) return;

    const uint32_t index = handle.handleIndex();
    auto& slot = _contracts[index];

    const bool isMainThread = (slot.executionType == ExecutionType::MainThread);

    // Drop work; we are not executing
    slot.work = nullptr;

    // Defensive clear target, resolved before the slot is freed
    auto& ownQueue = readyTreeFor(slot);

    // Invalidate handles and free the slot
    slot.generation.fetch_add(1, std::memory_order_acq_rel);
    slot.state.store(ContractState::Free, std::memory_order_release);

    // Defensive clear to keep signal tree clean
    ownQueue.clear(index);

    // Decrement active BEFORE returning to freelist
    _activeCount.fetch_sub(1, std::memory_order_acq_rel);

    // Return slot to freelist (ABA-resistant)
    auto packHead = [](uint32_t idx, uint32_t tag) -> uint64_t {
        return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
    };
    auto headIndex = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h & 0xFFFFFFFFull); };
    auto headTag = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h >> 32); };

    uint64_t old = _freeListHead.load(std::memory_order_acquire);
    for (;;) {
        slot.nextFree.store(headIndex(old), std::memory_order_release);
        uint64_t newH = packHead(index, headTag(old) + 1);
        if (_freeListHead.compare_exchange_weak(old, newH, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    // Final group access: decrement executing under _waitMutex and notify while
    // holding it (see executeContract for the rationale).
    {
        std::lock_guard<std::mutex> lock(_waitMutex);
        size_t newExecCount = isMainThread ? _mainThreadExecutingCount.fetch_sub(1, std::memory_order_acq_rel) - 1
                                           : _executingCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (newExecCount == 0) {
            _waitCondition.notify_all();
        }
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
    // Per-group zone, NAMED by the group (ZoneName) so Tracy aggregates the
    // per-frame main-thread "gap" by which subsystem's queue is spending it.
    // CpuZoneScope mirrors the same per-group split into `cpu.zones` (_name
    // outlives this scope; the profiler copies it on accumulate).
    ZoneScoped;
    ZoneName(_name.c_str(), _name.size());
    ::EntropyEngine::Core::Debug::CpuZoneScope _cpuz(_name.c_str());
    size_t executed = 0;
    uint64_t localBias = 0;

    while (executed < maxContracts) {
        auto handle = selectForMainThreadExecution(std::ref(localBias));
        if (!handle.valid()) {
            break;  // No more main thread contracts scheduled
        }

        // Execute the contract (includes all cleanup). Per-contract timing comes
        // from the zone inside executeContract (covers main + background paths).
        executeContract(handle);
        executed++;

        // Rotate bias to ensure fairness
        localBias = (localBias << 1) | (localBias >> 63);
    }

    return executed;
}

void WorkContractGroup::stop() {
    _stopDepth.fetch_add(1, std::memory_order_seq_cst);
    // Notify under the mutex: a wait() that evaluated its predicate before the
    // increment above must be blocked (not between check and block) when this
    // notify fires, or the wakeup is lost and wait() strands until the next event.
    std::lock_guard<std::mutex> lock(_waitMutex);
    _waitCondition.notify_all();
}

void WorkContractGroup::resume() {
    // Balance one stop(). Clamp (and complain) on unbalanced resumes rather
    // than letting the depth go negative and wedge isStopping() semantics.
    int64_t previous = _stopDepth.fetch_sub(1, std::memory_order_seq_cst);
    if (previous <= 0) {
        _stopDepth.fetch_add(1, std::memory_order_seq_cst);
        ENTROPY_LOG_WARNING_CAT("WorkContractGroup", "resume() without a matching stop(); ignoring");
    }
    // Note: We don't notify here - the caller should use their
    // concurrency provider to notify of available work if needed
}

void WorkContractGroup::wait() {
    // Use condition variable for efficient waiting instead of busy-wait
    std::unique_lock<std::mutex> lock(_waitMutex);
    _waitCondition.wait(lock, [this]() {
        if (_stopDepth.load(std::memory_order_seq_cst) > 0) {
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
    slot.work = nullptr;

    // Signal tree clearing strategy (triple-redundancy for correctness):
    // Layer 1: Primary clear immediately after selection (selectForExecution/selectForMainThreadExecution)
    // Layer 2: Scheduled cleanup - clear if released before execution starts
    // Layer 3: Defensive clear - handles race where selection thread was preempted before clearing
    // This ensures no stale ready bits remain in the signal tree regardless of thread scheduling.

    // Layer 2: Clear if contract was released while still scheduled (never selected for execution)
    // Layer 3: Defensive clear for Executing - handles the race where selectForExecution()
    // transitioned the state but was preempted before its Layer 1 clear:
    //   1. Thread A: select() returns index N, CAS Scheduled->Executing succeeds
    //   2. Thread A: preempted before the Layer 1 clear
    //   3. Thread B: executeContract(N) + completeExecution(N)
    //   4. Without this clear: signal tree still has stale bit N set
    if (previousState == ContractState::Scheduled || previousState == ContractState::Executing) {
        readyTreeFor(slot).clear(index);
    }

    // Always decrement active count BEFORE exposing slot to free list to avoid transient
    // activeCount > capacity windows under contention.
    auto newActiveCount = _activeCount.fetch_sub(1, std::memory_order_acq_rel) - 1;

    // Now push the slot back onto the free list so new createContract() can reuse it (ABA-resistant)
    auto packHead = [](uint32_t idx, uint32_t tag) -> uint64_t {
        return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
    };
    auto headIndex = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h & 0xFFFFFFFFull); };
    auto headTag = [](uint64_t h) -> uint32_t { return static_cast<uint32_t>(h >> 32); };

    uint64_t old = _freeListHead.load(std::memory_order_acquire);
    for (;;) {
        uint32_t oldIdx = headIndex(old);
        slot.nextFree.store(oldIdx, std::memory_order_release);
        uint64_t newH = packHead(index, headTag(old) + 1);
        if (_freeListHead.compare_exchange_weak(old, newH, std::memory_order_acq_rel, std::memory_order_acquire)) {
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

    // Final group access: the scheduled/executing decrement can make wait()'s
    // predicate true and release a waiter (including the destructor), so it is
    // performed under _waitMutex, after every other touch of the group, with the
    // notify issued while still holding the mutex.
    if (previousState == ContractState::Scheduled || previousState == ContractState::Executing) {
        std::lock_guard<std::mutex> lock(_waitMutex);
        size_t newCount;
        if (previousState == ContractState::Scheduled) {
            newCount = isMainThread ? _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1
                                    : _scheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        } else {
            newCount = isMainThread ? _mainThreadExecutingCount.fetch_sub(1, std::memory_order_acq_rel) - 1
                                    : _executingCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        }
        if (newCount == 0) {
            _waitCondition.notify_all();
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
    static const uint64_t hash =
        static_cast<uint64_t>(EntropyEngine::Core::TypeSystem::createTypeId<WorkContractGroup>().id);
    return hash;
}

std::string WorkContractGroup::toString() const {
    // Include name and capacity for quick identification
    return std::format("{}@{}(name=\"{}\", cap={})", className(), static_cast<const void*>(this), _name, _capacity);
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
    const bool stopping = _stopDepth.load(std::memory_order_relaxed) > 0;
    const bool hasProvider = (_concurrencyProvider != nullptr);

    return std::format(
        "{} [refs:{} active:{} sched:{} exec:{} sel:{} mainSched:{} mainExec:{} mainSel:{} stopping:{} provider:{}]",
        toString(), refCount(), active, sched, exec, sel, mainSched, mainExec, mainSel, stopping, hasProvider);
}

std::string WorkContractGroup::description() const {
    // For now, same as debugString for richer description
    return debugString();
}

size_t WorkContractGroup::checkTimedDeferrals() {
    std::lock_guard<std::mutex> lock(_timedDeferralCallbackMutex);
    size_t scheduled = 0;
    for (const auto& callback : _timedDeferralCallbacks) {
        if (callback) {
            scheduled += callback();
        }
    }
    return scheduled;
}

WorkContractGroup::TimedDeferralCallback WorkContractGroup::addTimedDeferralCallback(std::function<size_t()> callback) {
    std::lock_guard<std::mutex> lock(_timedDeferralCallbackMutex);
    _timedDeferralCallbacks.push_back(std::move(callback));
    return std::prev(_timedDeferralCallbacks.end());
}

void WorkContractGroup::removeTimedDeferralCallback(TimedDeferralCallback it) {
    std::lock_guard<std::mutex> lock(_timedDeferralCallbackMutex);
    _timedDeferralCallbacks.erase(it);
}

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
