/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file WorkContractHandle.h
 * @brief EntropyObject-based handle for scheduling and managing work contracts
 *
 * WorkContractHandle is an EntropyObject stamped with (owner + index + generation)
 * by WorkContractGroup. It exposes a safe API for scheduling, cancellation, and status
 * queries while preserving generation-based validation to prevent use-after-free.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "../Core/EntropyObject.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

// Forward declaration
class WorkContractGroup;

// Tag type for work contract handles
struct WorkContractTag
{
};

/**
 * @brief States that a work contract can be in during its lifecycle
 */
enum class ContractState : uint32_t
{
    Free = 0,       ///< Contract slot is available for allocation
    Allocated = 1,  ///< Contract has been allocated but not scheduled
    Scheduled = 2,  ///< Contract is scheduled and ready for execution
    Executing = 3,  ///< Contract is currently being executed
    Completed = 4,       ///< Contract has completed execution
    Peeking = 5,         ///< Scheduled; a worker is inspecting it to decide whether it may execute
    PeekReleased = 6,    ///< Released while peeked; the peeking worker frees the slot
    PeekUnscheduled = 7  ///< Unscheduled while peeked; the peeking worker returns it to Allocated
};

/**
 * @brief Result of schedule/unschedule operations
 */
enum class ScheduleResult
{
    Scheduled,         ///< Contract is now scheduled (successful schedule operation)
    AlreadyScheduled,  ///< Contract was already scheduled (schedule operation failed)
    NotScheduled,      ///< Contract is not scheduled (successful unschedule operation)
    Executing,         ///< Cannot modify - currently executing
    Invalid,           ///< Invalid handle provided
    TryAgainLater      ///< An unschedule is still being completed by a peeking worker; retry
};

/**
 * @class WorkContractHandle
 * @brief EntropyObject-stamped handle for work contracts
 *
 * This handle derives from EntropyObject and carries a stamped identity:
 * owner (WorkContractGroup*), slot index, and generation. The group is the
 * source of truth; validation compares the stamp against the group's slot.
 *
 * Copy semantics:
 * - Copying a handle copies only its stamped identity (no ownership transfer).
 * - The group owns lifetime; when a slot is freed, the handle becomes invalid.
 *
 * Typical workflow:
 * 1. Create via WorkContractGroup::createContract()
 * 2. Call schedule(), optionally unschedule()
 * 3. After execution starts or release(), valid() becomes false
 *
 * @code
 * WorkContractGroup group(1024);
 * auto h = group.createContract([]{ doWork(); });
 * if (h.schedule() == ScheduleResult::Scheduled) { // queued }
 * if (h.valid()) { // still schedulable }
 * @endcode
 */
class WorkContractHandle : public EntropyEngine::Core::EntropyObject
{
private:
    friend class WorkContractGroup;

    // Private constructor for group to stamp identity
    WorkContractHandle(WorkContractGroup* group, uint32_t index, uint32_t generation) {
        HandleAccess::set(*this, group, index, generation);
    }

public:
    // Default: invalid (no stamped identity)
    WorkContractHandle() = default;

    // Copy constructor: create a new handle object stamped with the same identity
    WorkContractHandle(const WorkContractHandle& other) noexcept {
        if (other.hasHandle()) {
            HandleAccess::set(*this, const_cast<void*>(other.handleOwner()), other.handleIndex(),
                              other.handleGeneration());
        }
    }
    // Copy assignment
    WorkContractHandle& operator=(const WorkContractHandle& other) noexcept {
        if (this != &other) {
            if (other.hasHandle()) {
                HandleAccess::set(*this, const_cast<void*>(other.handleOwner()), other.handleIndex(),
                                  other.handleGeneration());
            } else {
                HandleAccess::clear(*this);
            }
        }
        return *this;
    }
    // Move constructor
    WorkContractHandle(WorkContractHandle&& other) noexcept {
        if (other.hasHandle()) {
            HandleAccess::set(*this, const_cast<void*>(other.handleOwner()), other.handleIndex(),
                              other.handleGeneration());
        }
    }
    // Move assignment
    WorkContractHandle& operator=(WorkContractHandle&& other) noexcept {
        if (this != &other) {
            if (other.hasHandle()) {
                HandleAccess::set(*this, const_cast<void*>(other.handleOwner()), other.handleIndex(),
                                  other.handleGeneration());
            } else {
                HandleAccess::clear(*this);
            }
        }
        return *this;
    }

    // Maintain the same public API

    /**
     * @brief Schedules this contract for execution
     *
     * Transitions Allocated -> Scheduled. No-op if already scheduled.
     * @return Scheduled, AlreadyScheduled, Executing, Invalid, or TryAgainLater right after an
     *         unschedule() that raced a worker's peek
     *
     * @code
     * auto h = group.createContract([]{});
     * if (h.schedule() == ScheduleResult::Scheduled) { // scheduled }
     * @endcode
     */
    ScheduleResult schedule();

    /**
     * @brief Schedules this contract to run no earlier than a steady_clock time
     *
     * Transitions Allocated -> Scheduled like schedule(). A worker that pulls
     * the contract before @p due leaves it scheduled and pulls another.
     * WorkContractGroup::wait() waits for it like any scheduled contract.
     *
     * @param due Earliest time the contract may run; time_point::max() never becomes due
     * @return As schedule()
     *
     * @code
     * auto h = group.createContract([]{ heartbeat(); });
     * h.scheduleAt(std::chrono::steady_clock::now() + std::chrono::seconds(1));
     * @endcode
     */
    ScheduleResult scheduleAt(std::chrono::steady_clock::time_point due);

    /**
     * @brief Schedules this contract to become due after @p delay
     *
     * Same as scheduleAt(now + delay); saturates at time_point::max().
     *
     * @param delay Minimum time before the contract may run
     * @return As schedule()
     */
    ScheduleResult scheduleAfter(std::chrono::steady_clock::duration delay);

    /**
     * @brief Attempts to remove this contract from the ready set
     *
     * Succeeds only when in Scheduled state; cannot cancel while Executing.
     * @return NotScheduled on success, Executing if too late, or Invalid
     */
    ScheduleResult unschedule();

    /**
     * @brief Checks whether this handle still refers to a live slot
     * @return true if owner, index, and generation match a live slot
     */
    bool valid() const;

    /**
     * @brief Immediately frees this contract's slot
     *
     * Clears scheduling state and returns the slot to the free list. After this, valid() is false.
     */
    void release();

    /**
     * @brief Reports whether the contract is currently Scheduled
     * @return true if scheduled and waiting for execution, including while a worker peeks it
     */
    bool isScheduled() const;

    /**
     * @brief Reports whether the contract is currently Executing
     * @return true if actively running
     */
    bool isExecuting() const;

    const char* className() const noexcept override {
        return "WorkContractHandle";
    }
    uint64_t classHash() const noexcept override;
    std::string toString() const override;
};

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
