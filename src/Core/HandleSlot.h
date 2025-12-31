/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file HandleSlot.h
 * @brief Reusable generation-based handle validation utilities for slot-based pools
 *
 * This header provides common utilities for implementing the generation-based
 * handle pattern used throughout the Entropy Engine. The pattern prevents
 * use-after-free bugs by stamping EntropyObjects with a generation counter
 * that is incremented when slots are reused.
 *
 * The pattern works as follows:
 * 1. Each pool slot maintains an authoritative generation counter
 * 2. When an object is allocated from a slot, it's stamped with the slot's current generation
 * 3. Validation compares the object's stamped generation against the slot's current generation
 * 4. When released, the slot's generation increments, invalidating any stale references
 *
 * @code
 * // Example pool implementation using HandleSlot utilities
 * struct MySlot {
 *     SlotGeneration generation;           // Provides generation counter
 *     RefObject<MyObject> object;          // The pooled object
 *     bool active = false;                 // Slot state
 * };
 *
 * // Allocate
 * slot.active = true;
 * HandleSlotOps::stamp(*slot.object, this, index, slot.generation);
 *
 * // Validate
 * if (HandleSlotOps::validate(obj, this, index, slot.generation)) {
 *     // Object is valid for this slot
 * }
 *
 * // Release
 * HandleSlotOps::release(*slot.object, slot.generation);
 * slot.active = false;
 * @endcode
 *
 * @see EntropyObject for the base object with handle stamping support
 * @see HandleAccess for lower-level stamping primitives
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "EntropyObject.h"

namespace EntropyEngine::Core
{

/**
 * @brief Generation counter component for slot-based pools
 *
 * Provides the authoritative generation counter that lives in each pool slot.
 * The generation is incremented each time the slot is released, invalidating
 * any handles that were stamped with the previous generation.
 *
 * This is designed to be composed into custom slot structures:
 * @code
 * struct MyPoolSlot {
 *     SlotGeneration generation;    // Generation tracking
 *     RefObject<MyType> resource;   // The pooled resource
 *     // ... other slot-specific data
 * };
 * @endcode
 */
struct SlotGeneration
{
    std::atomic<uint32_t> value{1};  ///< Generation counter (starts at 1, 0 reserved for "never allocated")

    SlotGeneration() noexcept = default;

    // Non-copyable due to atomic, but movable for container operations
    SlotGeneration(const SlotGeneration&) = delete;
    SlotGeneration& operator=(const SlotGeneration&) = delete;

    SlotGeneration(SlotGeneration&& other) noexcept : value(other.value.load(std::memory_order_relaxed)) {}

    SlotGeneration& operator=(SlotGeneration&& other) noexcept {
        value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }

    /**
     * @brief Gets the current generation value
     *
     * Use this when stamping objects or for validation checks.
     *
     * @return Current generation value
     */
    [[nodiscard]] uint32_t current() const noexcept {
        return value.load(std::memory_order_acquire);
    }

    /**
     * @brief Increments the generation to invalidate stale handles
     *
     * Call this when releasing a slot. Any handles stamped with the
     * previous generation will fail validation after this call.
     *
     * @return The new generation value (post-increment)
     */
    uint32_t increment() noexcept {
        return value.fetch_add(1, std::memory_order_release) + 1;
    }

    /**
     * @brief Resets the generation (typically only used in tests or pool reinitialization)
     *
     * @param newValue Value to reset to (default: 1)
     */
    void reset(uint32_t newValue = 1) noexcept {
        value.store(newValue, std::memory_order_release);
    }
};

/**
 * @brief Operations for stamping, validating, and releasing handles in slot-based pools
 *
 * These helper functions encapsulate the common handle lifecycle operations,
 * ensuring consistent usage of the generation pattern across all pools.
 */
struct HandleSlotOps
{
    /**
     * @brief Stamps an EntropyObject with the slot's identity
     *
     * Records the owner pointer, slot index, and current generation on the object.
     * This establishes the object's "handle identity" for later validation.
     *
     * @tparam T EntropyObject-derived type
     * @param obj Object to stamp (must derive from EntropyObject)
     * @param owner Pool that owns this slot (typically 'this' pointer of the pool)
     * @param index Slot index within the pool
     * @param generation The slot's generation counter
     */
    template <typename T>
    static void stamp(T& obj, void* owner, uint32_t index, const SlotGeneration& generation) {
        static_assert(std::is_base_of_v<EntropyObject, T>,
                      "HandleSlotOps::stamp requires T to derive from EntropyObject");
        HandleAccess::set(obj, owner, index, generation.current());
    }

    /**
     * @brief Validates an object against a slot's current state
     *
     * Checks that:
     * 1. The object is not null
     * 2. The object has a valid handle stamp
     * 3. The object's owner matches the expected owner
     * 4. The object's index matches the expected index
     * 5. The object's stamped generation matches the slot's current generation
     *
     * @tparam T EntropyObject-derived type
     * @param obj Object to validate (may be null)
     * @param expectedOwner Expected owner pointer (typically 'this' pointer of the pool)
     * @param expectedIndex Expected slot index
     * @param generation The slot's generation counter
     * @return true if the object is valid for this slot
     */
    template <typename T>
    [[nodiscard]] static bool validate(const T* obj, const void* expectedOwner, uint32_t expectedIndex,
                                       const SlotGeneration& generation) noexcept {
        static_assert(std::is_base_of_v<EntropyObject, T>,
                      "HandleSlotOps::validate requires T to derive from EntropyObject");

        if (!obj) return false;
        if (!obj->hasHandle()) return false;
        if (obj->handleOwner() != expectedOwner) return false;
        if (obj->handleIndex() != expectedIndex) return false;
        return obj->handleGeneration() == generation.current();
    }

    /**
     * @brief Releases an object from a slot
     *
     * Clears the object's handle stamp and increments the slot's generation
     * to invalidate any stale references to this slot.
     *
     * @tparam T EntropyObject-derived type
     * @param obj Object to release
     * @param generation The slot's generation counter (will be incremented)
     */
    template <typename T>
    static void release(T& obj, SlotGeneration& generation) {
        static_assert(std::is_base_of_v<EntropyObject, T>,
                      "HandleSlotOps::release requires T to derive from EntropyObject");
        HandleAccess::clear(obj);
        generation.increment();
    }

    /**
     * @brief Clears an object's handle stamp without incrementing generation
     *
     * Use this when you need to clear the stamp but the slot is being reused
     * for a new object (e.g., replacing a render target with a different format).
     * The new object will be stamped with the same generation.
     *
     * @tparam T EntropyObject-derived type
     * @param obj Object to clear
     */
    template <typename T>
    static void clear(T& obj) {
        static_assert(std::is_base_of_v<EntropyObject, T>,
                      "HandleSlotOps::clear requires T to derive from EntropyObject");
        HandleAccess::clear(obj);
    }
};

/**
 * @brief Sentinel value for invalid slot indices
 *
 * Use this constant across all pools for consistency.
 */
constexpr uint32_t INVALID_SLOT_INDEX = ~0u;

}  // namespace EntropyEngine::Core
