/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace EntropyEngine
{
namespace Core
{
namespace TypeSystem
{
/**
 * @class GenericHandle
 * @brief Base class for type-safe handle implementations with generation-based validation.
 *
 * Provides a handle validation system that prevents use-after-free bugs
 * through generation counting. Each handle contains an index and generation packed into
 * a single 64-bit value, plus an optional owner reference.
 *
 * Focuses solely on handle representation and basic validation - does not
 * dictate storage mechanisms. Storage classes (pools, arrays, maps, etc.) are responsible
 * for implementing their own validation logic using the handle's index and generation.
 *
 * Features:
 * - Handle validation with owner reference support
 * - Generation-based validation to detect stale handles
 * - Type safety through templated derived classes
 * - Support for up to 4 billion objects with 4 billion generations each
 * - Storage-agnostic design for maximum flexibility
 *
 * The handle is packed as: [32-bit generation][32-bit index]
 * - Index 0 with generation 0 is reserved as invalid handle
 * - Maximum index: 4,294,967,295 (0xFFFFFFFF)
 * - Maximum generation: 4,294,967,295 (0xFFFFFFFF)
 *
 * Example usage with custom storage:
 * @code
 * class MyObjectManager {
 *     struct Slot { MyObject obj; uint32_t generation; bool occupied; };
 *     std::vector<Slot> slots;
 *
 *     bool isHandleValid(const MyObjectHandle& handle) const {
 *         uint32_t index = handle.getIndex();
 *         return index < slots.size() &&
 *                slots[index].occupied &&
 *                slots[index].generation == handle.getGeneration();
 *     }
 * };
 * @endcode
 */
template <typename OwnerType = void>
class GenericHandle
{
protected:
    OwnerType* _owner = nullptr;
    uint64_t _data;

    static constexpr uint64_t INDEX_MASK = 0xFFFFFFFF;
    static constexpr uint64_t GENERATION_MASK = 0xFFFFFFFF00000000;
    static constexpr uint32_t GENERATION_SHIFT = 32;
    static constexpr uint64_t INVALID_HANDLE = 0;

    /**
     * @brief Packs index and generation into a single 64-bit value
     */
    static constexpr uint64_t pack(uint32_t index, uint32_t generation) {
        return (static_cast<uint64_t>(generation) << GENERATION_SHIFT) | index;
    }

public:
    /**
     * @brief Default constructor creates an invalid handle
     */
    constexpr GenericHandle() : _owner(nullptr), _data(INVALID_HANDLE) {}

    /**
     * @brief Constructs a handle with the specified owner, index and generation
     * @param owner Pointer to the owning container
     * @param index Object index in the container
     * @param generation Generation counter for validation
     */
    constexpr GenericHandle(OwnerType* owner, uint32_t index, uint32_t generation)
        : _owner(owner), _data(pack(index, generation)) {}

    /**
     * @brief Constructs a handle with the specified owner and raw ID (for non-generation use)
     * @param owner Pointer to the owning container
     * @param id Raw 64-bit identifier
     */
    constexpr GenericHandle(OwnerType* owner, uint64_t id) : _owner(owner), _data(id) {}

    /**
     * @brief Gets the index component of this handle
     * @return The 32-bit index value
     */
    constexpr uint32_t getIndex() const {
        return static_cast<uint32_t>(_data & INDEX_MASK);
    }

    /**
     * @brief Gets the generation component of this handle
     * @return The 32-bit generation value
     */
    constexpr uint32_t getGeneration() const {
        return static_cast<uint32_t>(_data >> GENERATION_SHIFT);
    }

    /**
     * @brief Checks if this handle is potentially valid
     * @return true if the handle is not the invalid handle value and owner validation passes
     * @note For handles without owners, this only checks if the handle is non-null
     */
    constexpr bool isValid() const {
        if constexpr (std::is_same_v<OwnerType, void>) {
            return _data != INVALID_HANDLE;
        } else {
            return _owner && _data != INVALID_HANDLE;
        }
    }

    /**
     * @brief Invalidates this handle
     */
    void invalidate() {
        _data = INVALID_HANDLE;
    }

    /**
     * @brief Gets the raw packed data
     * @return The 64-bit packed handle data
     */
    constexpr uint64_t getRawData() const {
        return _data;
    }

    /**
     * @brief Gets the raw data as a 64-bit ID (for non-generation use cases)
     * @return The 64-bit ID value
     */
    constexpr uint64_t getId() const {
        return _data;
    }

    /**
     * @brief Gets the owner of this handle
     * @return Pointer to the owning container
     */
    constexpr OwnerType* getOwner() const {
        return _owner;
    }

    /**
     * @brief Equality comparison
     */
    constexpr bool operator==(const GenericHandle& other) const {
        return _owner == other._owner && _data == other._data;
    }

    /**
     * @brief Inequality comparison
     */
    constexpr bool operator!=(const GenericHandle& other) const {
        return !(*this == other);
    }

    /**
     * @brief Less-than comparison for use in sorted containers
     */
    constexpr bool operator<(const GenericHandle& other) const {
        if (_owner != other._owner) {
            return _owner < other._owner;
        }
        return _data < other._data;
    }

    /**
     * @brief Hash support for use in unordered containers
     */
    struct Hash
    {
        size_t operator()(const GenericHandle& handle) const {
            auto h1 = std::hash<OwnerType*>{}(handle._owner);
            auto h2 = std::hash<uint64_t>{}(handle._data);
            return h1 ^ (h2 << 1);
        }
    };
};

/**
 * @class TypedHandle
 * @brief Type-safe handle template that derives from GenericHandle
 * @tparam T The tag type this handle represents (usually an empty struct)
 * @tparam OwnerType The type of the owning container (optional)
 *
 * This template provides type safety on top of GenericHandle, preventing
 * accidental mixing of handles to different types of objects. The template
 * parameter T is typically a tag struct that exists solely for type safety.
 *
 * The handle is storage-agnostic - it only provides the index/generation
 * validation mechanism. Storage classes implement their own validation
 * logic using the handle's getIndex() and getGeneration() methods.
 *
 * Example usage:
 * @code
 * struct EntityTag {};
 * struct ComponentTag {};
 *
 * using EntityHandle = TypedHandle<EntityTag, EntityManager>;
 * using ComponentHandle = TypedHandle<ComponentTag, ComponentManager>;
 *
 * EntityManager entityMgr;
 * EntityHandle entity(&entityMgr, 5, 1);
 * ComponentHandle component(&componentMgr, 5, 1);
 * // entity == component would be a compile error (different types)
 *
 * // Usage with custom storage:
 * class EntityManager {
 *     bool isHandleValid(const EntityHandle& handle) const {
 *         return validateWithGeneration(handle.getIndex(), handle.getGeneration());
 *     }
 * };
 * @endcode
 */
template <typename T, typename OwnerType = void>
class TypedHandle : public GenericHandle<OwnerType>
{
public:
    using Type = T;

    /**
     * @brief Default constructor creates an invalid handle
     */
    constexpr TypedHandle() : GenericHandle<OwnerType>() {}

    /**
     * @brief Constructs a typed handle with the specified owner, index and generation
     */
    constexpr TypedHandle(OwnerType* owner, uint32_t index, uint32_t generation)
        : GenericHandle<OwnerType>(owner, index, generation) {}

    /**
     * @brief Constructs a typed handle with the specified owner and raw ID
     */
    constexpr TypedHandle(OwnerType* owner, uint64_t id) : GenericHandle<OwnerType>(owner, id) {}

    /**
     * @brief Constructs from a generic handle (explicit to prevent accidental conversion)
     */
    explicit constexpr TypedHandle(const GenericHandle<OwnerType>& generic) : GenericHandle<OwnerType>(generic) {}

    /**
     * @brief Creates an invalid handle of this type
     */
    static constexpr TypedHandle invalid() {
        return TypedHandle();
    }

    /**
     * @brief Gets a debug identifier for this handle (for logging/debugging)
     * @return Unique identifier combining index and generation
     */
    uint64_t getDebugId() const {
        return GenericHandle<OwnerType>::getRawData();
    }

    /**
     * @brief Hash support for use in unordered containers
     */
    struct Hash
    {
        size_t operator()(const TypedHandle& handle) const {
            return typename GenericHandle<OwnerType>::Hash{}(handle);
        }
    };
};
}  // namespace TypeSystem
}  // namespace Core
}  // namespace EntropyEngine

// Add standard library hash support for TypedHandle
namespace std
{
template <typename T, typename OwnerType>
struct hash<EntropyEngine::Core::TypeSystem::TypedHandle<T, OwnerType>>
{
    size_t operator()(const EntropyEngine::Core::TypeSystem::TypedHandle<T, OwnerType>& handle) const {
        return typename EntropyEngine::Core::TypeSystem::TypedHandle<T, OwnerType>::Hash{}(handle);
    }
};
}  // namespace std
