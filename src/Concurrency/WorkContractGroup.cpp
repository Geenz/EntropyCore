/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "WorkContractGroup.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <iostream>
#include <limits>
#include <thread>
#include <type_traits>

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
static_assert(sizeof(std::chrono::steady_clock::rep) == sizeof(int64_t) &&
                  std::is_signed_v<std::chrono::steady_clock::rep>,
              "ContractSlot::dueNs stores steady_clock ticks in an int64_t");

static int64_t toTicks(std::chrono::steady_clock::time_point t) noexcept {
    return static_cast<int64_t>(t.time_since_epoch().count());
}

static std::chrono::steady_clock::time_point fromTicks(int64_t ticks) noexcept {
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(ticks));
}

/// Packs a slot generation (high 32 bits) and state (low 32 bits) into one word.
static constexpr uint64_t packSlot(uint32_t generation, ContractState state) noexcept {
    return (static_cast<uint64_t>(generation) << 32) | static_cast<uint32_t>(state);
}

/// Generation half of a packed slot word.
static constexpr uint32_t slotGeneration(uint64_t word) noexcept {
    return static_cast<uint32_t>(word >> 32);
}

/// State half of a packed slot word.
static constexpr ContractState slotState(uint64_t word) noexcept {
    return static_cast<ContractState>(static_cast<uint32_t>(word));
}

/// True if a contract with due time @p dueNs (0: none) may run. Reads the clock at most
/// once per @p nowNs, which caches it for one selection pass (0 until first read).
static bool isDue(int64_t dueNs, int64_t& nowNs) noexcept {
    if (dueNs == 0) return true;
    if (nowNs == 0) nowNs = toTicks(std::chrono::steady_clock::now());
    return dueNs <= nowNs;
}

/// Peeking or one of its handoff states: the peeking worker owns the next transition.
static bool isPeekState(ContractState state) noexcept {
    return state == ContractState::Peeking || state == ContractState::PeekReleased ||
           state == ContractState::PeekUnscheduled;
}

/// Result of a schedule whose CAS from (@p generation, Allocated) found @p word instead.
static ScheduleResult scheduleFailure(uint64_t word, uint32_t generation) noexcept {
    if (slotGeneration(word) != generation) {
        return ScheduleResult::Invalid;  // the handle's contract is gone
    }
    switch (slotState(word)) {
        case ContractState::Scheduled:
        case ContractState::Peeking:
            return ScheduleResult::AlreadyScheduled;
        case ContractState::Executing:
            return ScheduleResult::Executing;
        case ContractState::PeekUnscheduled:
            return ScheduleResult::TryAgainLater;  // the peeking worker has not returned it to Allocated yet
        default:
            return ScheduleResult::Invalid;
    }
}

/// Reads a packed slot word, yielding while a worker holds the slot in a peek state.
static uint64_t loadSettledWord(const std::atomic<uint64_t>& word) {
    uint64_t current = word.load(std::memory_order_acquire);
    while (isPeekState(slotState(current))) {
        std::this_thread::yield();
        current = word.load(std::memory_order_acquire);
    }
    return current;
}

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

    // Every slot starts Free at generation 1
    for (auto& slot : _contracts) {
        slot.word.store(packSlot(1, ContractState::Free), std::memory_order_relaxed);
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

        // Check if this slot is occupied (not free). After stop() and wait() no worker is
        // peeking; a caller pumping the group outside a provider may still be.
        uint64_t word = loadSettledWord(slot.word);
        const ContractState currentState = slotState(word);

        if (currentState != ContractState::Free) {
            // Free it with a new generation in one step
            if (slot.word.compare_exchange_strong(word, packSlot(slotGeneration(word) + 1, ContractState::Free),
                                                  std::memory_order_acq_rel)) {
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

        // Check if this slot is scheduled; see releaseAllContracts() for the peek wait
        uint64_t word = loadSettledWord(slot.word);

        if (slotState(word) == ContractState::Scheduled) {
            // Try to transition from Scheduled to Allocated
            if (slot.word.compare_exchange_strong(word, packSlot(slotGeneration(word), ContractState::Allocated),
                                                  std::memory_order_acq_rel)) {
                // Remove from the slot's own ready queue
                bool isMainThread = (slot.executionType == ExecutionType::MainThread);
                readyTreeFor(slot).clear(i);
                clearDue(slot);

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

    // Validate that all contracts have been properly cleaned up. After
    // notifyGroupDestroyed() no provider thread can reach the group; before it, a
    // registered worker may still enter selection and return on the stop check.
    ENTROPY_DEBUG_BLOCK(
        size_t activeCount = _activeCount.load(std::memory_order_acquire);
        ENTROPY_ASSERT(activeCount == 0, "WorkContractGroup destroyed with active contracts still allocated");

        // Double-check that no threads are still selecting
        size_t selectingCount = _selectingCount.load(std::memory_order_acquire);
        ENTROPY_ASSERT(selectingCount == 0, "WorkContractGroup destroyed with threads still in selectForExecution");

        size_t mainThreadSelectingCount = _mainThreadSelectingCount.load(std::memory_order_acquire);
        ENTROPY_ASSERT(mainThreadSelectingCount == 0,
                       "WorkContractGroup destroyed with threads still in selectForMainThreadExecution"););
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

            // The popped slot is Free and exclusively ours; its generation stamps the handle
            const uint32_t generation = slotGeneration(slot.word.load(std::memory_order_acquire));

            // Assign work with noexcept wrapper to ensure termination on exceptions
            slot.work = [fn = std::move(work)]() noexcept {
                if (fn) fn();
            };
            slot.executionType = executionType;
            slot.pinnedLane = (executionType == ExecutionType::PinnedThread) ? pinnedLane : INVALID_INDEX;
            slot.dueNs.store(0, std::memory_order_relaxed);

            // Increment active count BEFORE making the slot visible as allocated.
            // This ensures that any thread that successfully observes the Allocated state
            // (via acquire) also observes the increased activeCount due to release/acquire
            // synchronization on slot.word.
            _activeCount.fetch_add(1, std::memory_order_acq_rel);
            // Transition state to allocated
            slot.word.store(packSlot(generation, ContractState::Allocated), std::memory_order_release);

            return WorkContractHandle(this, static_cast<uint32_t>(index), generation);
        }
        // CAS failed; head updated; retry
    }
}

ScheduleResult WorkContractGroup::scheduleContract(const WorkContractHandle& handle) {
    if (!validateHandle(handle)) return ScheduleResult::Invalid;

    uint32_t index = handle.handleIndex();
    auto& slot = _contracts[index];
    const uint32_t generation = handle.handleGeneration();

    // One CAS on generation and state: a stale handle cannot schedule another contract.
    uint64_t expected = packSlot(generation, ContractState::Allocated);
    if (!slot.word.compare_exchange_strong(expected, packSlot(generation, ContractState::Scheduled),
                                           std::memory_order_acq_rel)) {
        return scheduleFailure(expected, generation);
    }

    publishScheduled(index, slot);
    return ScheduleResult::Scheduled;
}

void WorkContractGroup::publishScheduled(uint32_t index, ContractSlot& slot) {
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
    std::shared_lock<std::shared_mutex> lock(_concurrencyProviderMutex);
    if (_concurrencyProvider) {
        if (slot.executionType == ExecutionType::MainThread) {
            _concurrencyProvider->notifyMainThreadWorkAvailable(this);
        } else {
            _concurrencyProvider->notifyWorkAvailableFor(this, slot.executionType, slot.pinnedLane);
        }
    }
}

ScheduleResult WorkContractGroup::scheduleContractAt(const WorkContractHandle& handle,
                                                     std::chrono::steady_clock::time_point due) {
    if (due <= std::chrono::steady_clock::now()) {
        return scheduleContract(handle);
    }
    if (!validateHandle(handle)) return ScheduleResult::Invalid;

    uint32_t index = handle.handleIndex();
    auto& slot = _contracts[index];
    const uint32_t generation = handle.handleGeneration();

    // Allocated -> Executing while the due time is written: no selector or scheduler
    // can take the slot until the Scheduled store below publishes it.
    uint64_t expected = packSlot(generation, ContractState::Allocated);
    if (!slot.word.compare_exchange_strong(expected, packSlot(generation, ContractState::Executing),
                                           std::memory_order_acq_rel)) {
        return scheduleFailure(expected, generation);
    }

    slot.dueNs.store(toTicks(due), std::memory_order_relaxed);  // published by the Scheduled store
    _timedCount.fetch_add(1, std::memory_order_acq_rel);        // count before bit, as publishScheduled
    slot.word.store(packSlot(generation, ContractState::Scheduled), std::memory_order_release);
    publishScheduled(index, slot);
    return ScheduleResult::Scheduled;
}

SignalTreeBase& WorkContractGroup::treeFor(ExecutionType type, uint32_t lane) {
    if (type == ExecutionType::MainThread) return *_mainThreadContracts;
    if (type == ExecutionType::PinnedThread && lane < _pinnedLanes.size()) return *_pinnedLanes[lane];
    return *_readyContracts;
}

SignalTreeBase& WorkContractGroup::readyTreeFor(const ContractSlot& slot) {
    return treeFor(slot.executionType, slot.pinnedLane);
}

bool WorkContractGroup::hasMainThreadWork() const noexcept {
    if (_mainThreadScheduledCount.load(std::memory_order_acquire) == 0) {
        return false;
    }
    // Peek for a due contract; the first untimed one answers at once.
    int64_t nowNs = 0;
    for (size_t index = _mainThreadContracts->peek(0); index != SignalTreeBase::S_INVALID_SIGNAL_INDEX && index < _capacity;
         index = _mainThreadContracts->peek(index + 1)) {
        const ContractSlot& slot = _contracts[index];
        const ContractState state = slotState(slot.word.load(std::memory_order_acquire));
        if ((state == ContractState::Scheduled || state == ContractState::Peeking) &&
            isDue(slot.dueNs.load(std::memory_order_relaxed), nowNs)) {
            return true;
        }
    }
    return false;
}

bool WorkContractGroup::clearDue(ContractSlot& slot) noexcept {
    if (slot.dueNs.exchange(0, std::memory_order_relaxed) == 0) return false;
    _timedCount.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

bool WorkContractGroup::resolvePeek(uint32_t index, ContractSlot& slot, uint32_t generation, ContractState target) {
    const bool isMainThread = slot.executionType == ExecutionType::MainThread;
    uint64_t expected = packSlot(generation, ContractState::Peeking);
    if (slot.word.compare_exchange_strong(expected, packSlot(generation, target), std::memory_order_acq_rel)) {
        return true;
    }

    // Only this worker moves the slot out of Peeking or a handoff state, and the owner
    // can only hand over a release or an unschedule.
    if (slotState(expected) == ContractState::PeekUnscheduled) {
        // The owner's unschedule, completed here; Allocated last so a reschedule counts after this.
        readyTreeFor(slot).clear(index);
        clearDue(slot);
        {
            std::lock_guard<std::mutex> lock(_waitMutex);
            size_t newScheduledCount = isMainThread ? _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1
                                                    : _scheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (newScheduledCount == 0) {
                _waitCondition.notify_all();
            }
        }
        expected = packSlot(generation, ContractState::PeekUnscheduled);
        if (slot.word.compare_exchange_strong(expected, packSlot(generation, ContractState::Allocated),
                                              std::memory_order_acq_rel)) {
            return false;
        }
        // Released after the unschedule: the contract is already out of the queue and the
        // counts, so what remains is the release of an Allocated contract.
        ENTROPY_ASSERT(slotState(expected) == ContractState::PeekReleased,
                       "resolvePeek: unexpected unschedule handoff state");
        slot.word.store(packSlot(generation + 1, ContractState::Free), std::memory_order_release);
        returnSlotToFreeList(index, ContractState::Allocated, isMainThread);
        return false;
    }

    // The owner's release, completed here: nothing else can touch the slot until it is free.
    ENTROPY_ASSERT(slotState(expected) == ContractState::PeekReleased, "resolvePeek: unexpected peek handoff state");
    slot.word.store(packSlot(generation + 1, ContractState::Free), std::memory_order_release);
    returnSlotToFreeList(index, ContractState::Scheduled, isMainThread);
    return false;
}

void WorkContractGroup::clearStaleBit(ExecutionType type, uint32_t lane, uint32_t index, const ContractSlot& slot) {
    SignalTreeBase& tree = treeFor(type, lane);
    tree.clear(index);
    // Pairs with the release of a set() the clear may have read, so a schedule whose bit
    // was just cleared is visible below.
    std::atomic_thread_fence(std::memory_order_acquire);
    const ContractState state = slotState(slot.word.load(std::memory_order_acquire));
    if ((state == ContractState::Scheduled || state == ContractState::Peeking) && &readyTreeFor(slot) == &tree) {
        tree.set(index);
    }
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
    const uint32_t generation = handle.handleGeneration();

    uint64_t word = slot.word.load(std::memory_order_acquire);
    for (;;) {
        if (slotGeneration(word) != generation) {
            // Slot was freed/reused. It may be due to execution having started (unified flow).
            const ContractState st = slotState(word);
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

        switch (slotState(word)) {
            case ContractState::Scheduled:
                if (!slot.word.compare_exchange_weak(word, packSlot(generation, ContractState::Allocated),
                                                     std::memory_order_acq_rel, std::memory_order_acquire)) {
                    continue;  // a worker peeked or claimed it; re-read
                }
                {
                    // Remove from the slot's own ready queue
                    bool isMainThread = (slot.executionType == ExecutionType::MainThread);
                    readyTreeFor(slot).clear(index);
                    clearDue(slot);

                    // Decrement under _waitMutex and notify while holding it: this
                    // decrement can make wait()'s predicate true, so the waiter must not
                    // be able to observe it before we are done touching the group.
                    std::lock_guard<std::mutex> lock(_waitMutex);
                    size_t newScheduledCount =
                        isMainThread ? _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1
                                     : _scheduledCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    if (newScheduledCount == 0) {
                        _waitCondition.notify_all();
                    }
                }
                return ScheduleResult::NotScheduled;

            case ContractState::Peeking:
                // A worker is peeking it: hand the unschedule to that worker, which returns
                // the slot to Allocated instead of running it.
                if (!slot.word.compare_exchange_weak(word, packSlot(generation, ContractState::PeekUnscheduled),
                                                     std::memory_order_acq_rel, std::memory_order_acquire)) {
                    continue;
                }
                return ScheduleResult::NotScheduled;

            case ContractState::Allocated:
            case ContractState::PeekUnscheduled:
                return ScheduleResult::NotScheduled;

            case ContractState::Executing:
                return ScheduleResult::Executing;

            default:
                return ScheduleResult::Invalid;  // Free, or released during a peek
        }
    }
}

void WorkContractGroup::releaseContract(const WorkContractHandle& handle) {
    if (!validateHandle(handle)) return;

    uint32_t index = handle.handleIndex();

    // Bounds check to prevent out-of-bounds access
    if (index >= _capacity) return;

    auto& slot = _contracts[index];
    const uint32_t generation = handle.handleGeneration();
    const bool isMainThread = (slot.executionType == ExecutionType::MainThread);

    // Every CAS carries the handle's generation, so a stale handle never frees another contract.
    uint64_t word = slot.word.load(std::memory_order_acquire);
    while (slotGeneration(word) == generation) {
        const ContractState state = slotState(word);
        switch (state) {
            case ContractState::Allocated:
            case ContractState::Scheduled:
                // Free it with a new generation in one step; this is the race with selection
                if (slot.word.compare_exchange_weak(word, packSlot(generation + 1, ContractState::Free),
                                                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                    returnSlotToFreeList(index, state, isMainThread);
                    return;
                }
                continue;

            case ContractState::Peeking:
            case ContractState::PeekUnscheduled:
                // A worker is peeking it: hand the release to that worker, which frees the
                // slot instead of running it. The slot cannot be reused before then.
                if (slot.word.compare_exchange_weak(word, packSlot(generation, ContractState::PeekReleased),
                                                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return;
                }
                continue;

            default:
                // Executing, Free, or already handed over: this thread can no longer act.
                return;
        }
    }
}

bool WorkContractGroup::isValidHandle(const WorkContractHandle& handle) const noexcept {
    return validateHandle(handle);
}

WorkContractHandle WorkContractGroup::selectForExecution(std::optional<std::reference_wrapper<uint64_t>> bias,
                                                         std::chrono::steady_clock::time_point* nextDue) {
    // Empty-queue skip: one atomic load instead of the guard's _waitMutex round trip.
    // A stale read is possible right after a setter returns; the setter notifies after set(), so a worker that misses the bit is woken or finds a wake token when it parks.
    if (_readyContracts->isEmpty()) {
        return WorkContractHandle();
    }

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

    return claimFrom(ExecutionType::AnyThread, INVALID_INDEX, biasRef, nextDue);
}

WorkContractHandle WorkContractGroup::claimFrom(ExecutionType expectedType, uint32_t expectedLane, uint64_t& bias,
                                                std::chrono::steady_clock::time_point* nextDue) {
    SignalTreeBase& tree = treeFor(expectedType, expectedLane);
    const size_t signals = tree.getCapacity();
    const size_t start = static_cast<size_t>(bias % signals);
    const bool mainThread = expectedType == ExecutionType::MainThread;
    int64_t nowNs = 0;
    WorkContractHandle claimed;

    // Peek start -> end, then wrap once 0 -> start. Peeking takes no bit, so every other
    // worker keeps seeing the whole queue.
    size_t from = start;
    bool wrapped = false;
    for (;;) {
        const size_t index = tree.peek(from);
        if (!wrapped && index == SignalTreeBase::S_INVALID_SIGNAL_INDEX) {
            if (start == 0) break;
            wrapped = true;
            from = 0;
            continue;
        }
        if (wrapped && (index == SignalTreeBase::S_INVALID_SIGNAL_INDEX || index >= start)) {
            break;
        }
        from = index + 1;
        if (index >= _capacity) {
            continue;
        }

        const auto slotIndex = static_cast<uint32_t>(index);
        auto& slot = _contracts[index];

        uint64_t word = slot.word.load(std::memory_order_acquire);
        const ContractState state = slotState(word);
        if (state == ContractState::Free || state == ContractState::Allocated) {
            clearStaleBit(expectedType, expectedLane, slotIndex, slot);
            continue;
        }
        if (state != ContractState::Scheduled) {
            continue;  // an executing claim or another worker's peek owns the bit
        }

        const uint32_t generation = slotGeneration(word);
        if (!slot.word.compare_exchange_strong(word, packSlot(generation, ContractState::Peeking),
                                               std::memory_order_acq_rel)) {
            continue;
        }

        // Peeking: the owner can only hand a release or an unschedule to this worker, so
        // the queue and due time read below belong to the contract that would run.
        if (slot.executionType != expectedType ||
            (expectedType == ExecutionType::PinnedThread && slot.pinnedLane != expectedLane)) {
            // Stale bit from a previous occupant; the contract belongs to another queue.
            if (resolvePeek(slotIndex, slot, generation, ContractState::Scheduled)) {
                readyTreeFor(slot).set(index);
            }
            clearStaleBit(expectedType, expectedLane, slotIndex, slot);
            continue;
        }

        const int64_t due = slot.dueNs.load(std::memory_order_relaxed);
        if (!isDue(due, nowNs)) {
            if (resolvePeek(slotIndex, slot, generation, ContractState::Scheduled)) {
                if (nextDue) *nextDue = std::min(*nextDue, fromTicks(due));
            }
            continue;
        }

        if (!resolvePeek(slotIndex, slot, generation, ContractState::Executing)) {
            continue;  // released or unscheduled during the peek; the handoff is complete
        }

        // Clear from this queue immediately upon successful selection to avoid stale ready bits.
        // CRITICAL: This clear is part of a triple-redundancy strategy to ensure no stale bits remain
        // in the signal tree under any thread interleaving. See returnSlotToFreeList() for defensive
        // clear that handles the race where this thread is preempted before clearing.
        tree.clear(index);

        // Update counters: increment executing BEFORE decrementing scheduled, so
        // wait()'s conjunction (scheduled==0 && executing==0) can never observe the
        // claimed contract in neither counter mid-handoff and return early.
        // Pinned work shares the background counters.
        if (mainThread) {
            _mainThreadExecutingCount.fetch_add(1, std::memory_order_acq_rel);
            _mainThreadScheduledCount.fetch_sub(1, std::memory_order_acq_rel);
        } else {
            _executingCount.fetch_add(1, std::memory_order_acq_rel);
            _scheduledCount.fetch_sub(1, std::memory_order_acq_rel);
        }

        // A due contract was claimed and others are still timed: wake one more worker
        // to pull them. Pinned and main-thread timed work have a single drainer.
        if (clearDue(slot) && expectedType == ExecutionType::AnyThread &&
            _timedCount.load(std::memory_order_acquire) > 0) {
            std::shared_lock<std::shared_mutex> lock(_concurrencyProviderMutex);
            if (_concurrencyProvider) {
                _concurrencyProvider->notifyWorkAvailable(this);
            }
        }

        claimed = WorkContractHandle(this, slotIndex, generation);
        bias = index + 1;
        break;
    }

    return claimed;
}

WorkContractHandle WorkContractGroup::selectForPinnedExecution(size_t lane,
                                                               std::optional<std::reference_wrapper<uint64_t>> bias,
                                                               std::chrono::steady_clock::time_point* nextDue) {
    if (lane >= _pinnedLanes.size() || _pinnedLanes[lane]->isEmpty()) {
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

    return claimFrom(ExecutionType::PinnedThread, static_cast<uint32_t>(lane), biasRef, nextDue);
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
    std::optional<std::reference_wrapper<uint64_t>> bias, std::chrono::steady_clock::time_point* nextDue) {
    // See selectForExecution: skip registration entirely on an empty queue.
    if (_mainThreadContracts->isEmpty()) {
        return WorkContractHandle();
    }

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

    return claimFrom(ExecutionType::MainThread, INVALID_INDEX, biasRef, nextDue);
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
    // Invalidate handles and transition to Free in one store; the claim made the slot ours
    slot.word.store(packSlot(handle.handleGeneration() + 1, ContractState::Free), std::memory_order_release);

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

    // Invalidate handles and free the slot in one store; the claim made the slot ours
    slot.word.store(packSlot(handle.handleGeneration() + 1, ContractState::Free), std::memory_order_release);

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
    return slotGeneration(_contracts[index].word.load(std::memory_order_acquire)) == handle.handleGeneration();
}

ContractState WorkContractGroup::getContractState(const WorkContractHandle& handle) const noexcept {
    if (handle.handleOwner() != static_cast<const void*>(this) || handle.handleIndex() >= _capacity) {
        return ContractState::Free;
    }
    // One load: generation and state belong to the same occupancy.
    const uint64_t word = _contracts[handle.handleIndex()].word.load(std::memory_order_acquire);
    return slotGeneration(word) == handle.handleGeneration() ? slotState(word) : ContractState::Free;
}

size_t WorkContractGroup::executingCount() const noexcept {
    return _executingCount.load(std::memory_order_acquire);
}

void WorkContractGroup::returnSlotToFreeList(uint32_t index, ContractState previousState, bool isMainThread) {
    auto& slot = _contracts[index];

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

    if (previousState == ContractState::Scheduled) {
        clearDue(slot);
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
    const auto timed = _timedCount.load(std::memory_order_relaxed);
    const bool stopping = _stopDepth.load(std::memory_order_relaxed) > 0;
    const bool hasProvider = (_concurrencyProvider != nullptr);

    return std::format(
        "{} [refs:{} active:{} sched:{} exec:{} sel:{} mainSched:{} mainExec:{} mainSel:{} timed:{} stopping:{} "
        "provider:{}]",
        toString(), refCount(), active, sched, exec, sel, mainSched, mainExec, mainSel, timed, stopping, hasProvider);
}

std::string WorkContractGroup::description() const {
    // For now, same as debugString for richer description
    return debugString();
}

size_t WorkContractGroup::checkTimedDeferrals() {
    size_t scheduled = 0;
    std::lock_guard<std::mutex> lock(_timedDeferralCallbackMutex);
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
