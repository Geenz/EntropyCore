/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file SlotPool.h
 * @brief Reusable slot-based pool with generation-based handle validation
 *
 * SlotPool provides the common infrastructure for slot-based resource pools:
 * - Fixed-capacity slot array with lazy initialization
 * - Generation counters for stale handle detection
 * - Free list management for O(1) allocation
 * - Stamping/validation/release via EntropyObject handle system
 *
 * This template is designed to be composed with custom slot data, allowing
 * services to define their own per-slot fields while getting the pool
 * mechanics for free.
 *
 * @code
 * // Define custom slot data
 * struct MaterialSlotData {
 *     RefObject<Material> material;
 *     std::string name;
 * };
 *
 * // Use the pool
 * SlotPool<MaterialSlotData> pool(1024);
 *
 * // Allocate
 * auto [index, slot] = pool.allocate();
 * slot->material = makeRef<Material>();
 * pool.stamp(*slot->material, index);
 *
 * // Validate
 * if (pool.isValid(material, expectedIndex)) { ... }
 *
 * // Release
 * pool.release(*slot->material, index);
 * @endcode
 *
 * @see HandleSlot.h for lower-level generation utilities
 * @see EntropyObject for the handle stamping interface
 */

#pragma once

#include "HandleSlot.h"
#include "RefObject.h"
#include <vector>
#include <mutex>
#include <optional>

namespace EntropyEngine::Core {

/**
 * @brief Slot-based pool with generation-based handle validation
 *
 * @tparam SlotData User-defined data stored in each slot (will be default-constructed)
 *
 * Thread Safety:
 * - All public methods are thread-safe (protected by internal mutex)
 * - SlotData must be safe to access under the returned lock guard
 */
template<typename SlotData>
class SlotPool {
public:
    /**
     * @brief Internal slot structure combining generation tracking with user data
     */
    struct Slot {
        SlotGeneration generation;  ///< Authoritative generation counter
        SlotData data;              ///< User-defined slot data
        bool active = false;        ///< Slot is currently allocated
    };

    /**
     * @brief Result of allocate() containing slot index and data reference
     */
    struct AllocResult {
        uint32_t index;
        SlotData& data;
    };

    /**
     * @brief Constructs a pool with the given capacity
     *
     * Slots are default-constructed immediately. Use init() for lazy initialization
     * if SlotData requires external dependencies.
     *
     * @param capacity Maximum number of slots
     */
    explicit SlotPool(size_t capacity)
        : _slots(capacity)
        , _capacity(capacity)
    {
        // Initialize free list (all slots are free initially)
        _freeList.reserve(capacity);
        for (size_t i = 0; i < capacity; ++i) {
            _freeList.push_back(static_cast<uint32_t>(capacity - 1 - i));  // Push in reverse for LIFO
        }
    }

    /**
     * @brief Allocates a slot from the pool
     *
     * @return Optional containing (index, data reference), or nullopt if pool is full
     */
    [[nodiscard]] std::optional<AllocResult> allocate() {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_freeList.empty()) {
            return std::nullopt;
        }

        uint32_t index = _freeList.back();
        _freeList.pop_back();

        Slot& slot = _slots[index];
        slot.active = true;
        ++_activeCount;

        return AllocResult{index, slot.data};
    }

    /**
     * @brief Stamps an EntropyObject with this pool's identity
     *
     * Call this after allocate() to establish the object's handle identity.
     *
     * @tparam T EntropyObject-derived type
     * @param obj Object to stamp
     * @param index Slot index from allocate()
     */
    template<typename T>
    void stamp(T& obj, uint32_t index) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (index >= _capacity) return;
        HandleSlotOps::stamp(obj, this, index, _slots[index].generation);
    }

    /**
     * @brief Validates an object against its expected slot
     *
     * @tparam T EntropyObject-derived type
     * @param obj Object to validate (may be null)
     * @param index Expected slot index
     * @return true if object is valid for this slot
     */
    template<typename T>
    [[nodiscard]] bool isValid(const T* obj, uint32_t index) const {
        std::lock_guard<std::mutex> lock(_mutex);

        if (index >= _capacity) return false;
        const Slot& slot = _slots[index];
        if (!slot.active) return false;

        return HandleSlotOps::validate(obj, this, index, slot.generation);
    }

    /**
     * @brief Validates an object and returns its slot data if valid
     *
     * Convenience method that combines validation with data access.
     *
     * @tparam T EntropyObject-derived type
     * @param obj Object to validate
     * @return Pointer to slot data, or nullptr if invalid
     */
    template<typename T>
    [[nodiscard]] const SlotData* getIfValid(const T* obj) const {
        if (!obj || !obj->hasHandle()) return nullptr;
        if (obj->handleOwner() != this) return nullptr;

        std::lock_guard<std::mutex> lock(_mutex);

        uint32_t index = obj->handleIndex();
        if (index >= _capacity) return nullptr;

        const Slot& slot = _slots[index];
        if (!slot.active) return nullptr;
        if (obj->handleGeneration() != slot.generation.current()) return nullptr;

        return &slot.data;
    }

    /**
     * @brief Releases a slot back to the pool
     *
     * Clears the object's handle stamp and increments the slot's generation
     * to invalidate any stale references.
     *
     * @tparam T EntropyObject-derived type
     * @param obj Object to release
     * @param index Slot index
     */
    template<typename T>
    void release(T& obj, uint32_t index) {
        std::lock_guard<std::mutex> lock(_mutex);

        if (index >= _capacity) return;

        Slot& slot = _slots[index];
        if (!slot.active) return;

        HandleSlotOps::release(obj, slot.generation);
        slot.active = false;
        --_activeCount;
        _freeList.push_back(index);
    }

    /**
     * @brief Gets a slot's data by index (unsafe - no validation)
     *
     * Use isValid() first, or prefer getIfValid() for safe access.
     *
     * @param index Slot index
     * @return Reference to slot data
     */
    [[nodiscard]] SlotData& at(uint32_t index) {
        std::lock_guard<std::mutex> lock(_mutex);
        return _slots[index].data;
    }

    /**
     * @brief Gets a slot's data by index (const, unsafe - no validation)
     */
    [[nodiscard]] const SlotData& at(uint32_t index) const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _slots[index].data;
    }

    /**
     * @brief Gets a slot's current generation
     *
     * @param index Slot index
     * @return Current generation value
     */
    [[nodiscard]] uint32_t generation(uint32_t index) const {
        std::lock_guard<std::mutex> lock(_mutex);
        return index < _capacity ? _slots[index].generation.current() : 0;
    }

    /**
     * @brief Checks if a slot is currently active (allocated)
     *
     * @param index Slot index
     * @return true if slot is active
     */
    [[nodiscard]] bool isActive(uint32_t index) const {
        std::lock_guard<std::mutex> lock(_mutex);
        return index < _capacity && _slots[index].active;
    }

    // Statistics
    [[nodiscard]] size_t capacity() const noexcept { return _capacity; }
    [[nodiscard]] size_t activeCount() const noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        return _activeCount;
    }
    [[nodiscard]] size_t freeCount() const noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        return _freeList.size();
    }

    /**
     * @brief Iterates over all active slots
     *
     * @param fn Callback receiving (index, SlotData&) for each active slot
     */
    template<typename Fn>
    void forEachActive(Fn&& fn) {
        std::lock_guard<std::mutex> lock(_mutex);
        for (size_t i = 0; i < _capacity; ++i) {
            if (_slots[i].active) {
                fn(static_cast<uint32_t>(i), _slots[i].data);
            }
        }
    }

    /**
     * @brief Iterates over all active slots (const)
     */
    template<typename Fn>
    void forEachActive(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(_mutex);
        for (size_t i = 0; i < _capacity; ++i) {
            if (_slots[i].active) {
                fn(static_cast<uint32_t>(i), _slots[i].data);
            }
        }
    }

private:
    std::vector<Slot> _slots;
    std::vector<uint32_t> _freeList;
    size_t _capacity;
    size_t _activeCount = 0;
    mutable std::mutex _mutex;
};

} // namespace EntropyEngine::Core
