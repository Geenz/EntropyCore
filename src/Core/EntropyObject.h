/**
 * @file EntropyObject.h
 * @brief Reference-counted base object with optional stamped handle identity
 *
 * EntropyObject provides a lightweight, thread-safe reference counting base for
 * objects exposed across subsystem boundaries (C/C++/scripting). In addition to
 * retain/release semantics, an object may be stamped with a handle identity
 * (owner + index + generation) by a registry/owner to support generation-based
 * validation and interop with handle-centric APIs.
 *
 * @code
 * // Owner/registry stamps identity when allocating a slot
 * struct SlotOwner {
 *     void allocate(EntropyObject& obj, uint32_t index, uint32_t generation) {
 *         HandleAccess::set(obj, this, index, generation);
 *     }
 *     void release(EntropyObject& obj) {
 *         HandleAccess::clear(obj);
 *     }
 * };
 *
 * // Cross-boundary usage with retain/release
 * EntropyObject* shared = createObject();
 * shared->retain();   // Share safely
 * useObject(shared);
 * shared->release();  // Balance retains to avoid leaks
 * @endcode
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace EntropyEngine::Core
{

/**
 * @brief Memory profiling callbacks for EntropyObject lifecycle tracking
 *
 * These callbacks allow external profilers (e.g., Tracy) to track EntropyObject
 * allocations without adding profiler dependencies to EntropyCore.
 *
 * @code
 * // Example: Register Tracy callbacks in EntropyPortal
 * EntropyObjectMemoryHooks::onAlloc = [](void* ptr, size_t size, const char* name) {
 *     TracyAllocN(ptr, size, name);
 * };
 * EntropyObjectMemoryHooks::onFree = [](void* ptr, const char* name) {
 *     TracyFreeN(ptr, name);
 * };
 * @endcode
 */
struct EntropyObjectMemoryHooks
{
    using AllocCallback = void (*)(void* ptr, size_t size, const char* className);
    using FreeCallback = void (*)(void* ptr, const char* className);

    static inline AllocCallback onAlloc = nullptr;
    static inline FreeCallback onFree = nullptr;
};

// Forward declarations
namespace TypeSystem
{
class TypeInfo;
template <class OwnerType>
class GenericHandle;  // fwd decl for helpers (optional)
template <class T, class OwnerType>
class TypedHandle;  // fwd decl for helpers (optional)
}  // namespace TypeSystem

/**
 * @class EntropyObject
 * @brief Ref-counted base with optional handle stamping and basic introspection
 *
 * Designed for safe sharing across module/language boundaries. Reference count
 * operations are thread-safe. When stamped with a handle identity, the object
 * can participate in owner/index/generation validation without coupling to
 * a specific handle type.
 */
class EntropyObject
{
protected:
    mutable std::atomic<uint32_t> _refCount{1};  ///< Thread-safe retain/release counter

    /**
     * @brief Optional handle identity stamped by an owner/registry
     *
     * When set, identifies the object within its owner using index+generation.
     * This enables generation-based validation and C API interop.
     */
    struct HandleCore
    {
        void* owner = nullptr;    ///< Owning registry that stamped this object
        uint32_t index = 0;       ///< Slot index within the owner
        uint32_t generation = 0;  ///< Generation for stale-handle detection
        bool isSet() const noexcept {
            return owner != nullptr;
        }
        uint64_t id64() const noexcept {
            return (static_cast<uint64_t>(index) << 32) | generation;
        }
    } _handle{};

    // Internal setters used by owners/registries to stamp identity
    void _setHandleIdentity(void* owner, uint32_t index, uint32_t generation) noexcept {
        _handle.owner = owner;
        _handle.index = index;
        _handle.generation = generation;
    }
    void _clearHandleIdentity() noexcept {
        _handle = {};
    }

    // Grant access to helper
    friend struct HandleAccess;

public:
    EntropyObject() noexcept = default;
    EntropyObject(EntropyObject&&) = delete;
    EntropyObject(const EntropyObject&) = delete;
    EntropyObject& operator=(const EntropyObject&) = delete;
    EntropyObject& operator=(EntropyObject&&) = delete;

    virtual ~EntropyObject() noexcept = default;

    /**
     * @brief Increments the reference count
     * @note Thread-safe; may be called from any thread.
     */
    void retain() const noexcept;
    /**
     * @brief Attempts to retain only if the object is still alive
     *
     * Use when you need to safely grab a reference from a background thread
     * without racing destruction. If the refcount has already reached zero,
     * tryRetain() returns false and the object must not be used.
     *
     * @return true on success (reference acquired), false if object already dead
     *
     * @code
     * // Example: Try to use an object that might be concurrently released
     * if (obj->tryRetain()) {
     *     doWork(obj);
     *     obj->release();
     * } // else: object was already destroyed or being destroyed
     * @endcode
     */
    bool tryRetain() const noexcept;

    /**
     * @brief Decrements the reference count and deletes when it reaches zero
     * @note Thread-safe; may delete the object on the calling thread when count hits zero.
     */
    void release() const noexcept;

    /**
     * @brief Current reference count (approximate under contention)
     */
    uint32_t refCount() const noexcept;

    // Handle introspection (safe even if not stamped)
    /** @return true if an owner has stamped this object with handle identity */
    bool hasHandle() const noexcept {
        return _handle.isSet();
    }
    /** @return Owner pointer that stamped this object, or null if none */
    const void* handleOwner() const noexcept {
        return _handle.owner;
    }
    /** @return Index value stamped by the owner (undefined if !hasHandle()) */
    uint32_t handleIndex() const noexcept {
        return _handle.index;
    }
    /** @return Generation value stamped by the owner (undefined if !hasHandle()) */
    uint32_t handleGeneration() const noexcept {
        return _handle.generation;
    }
    /** @return 64-bit packed index:generation identifier (undefined if !hasHandle()) */
    uint64_t handleId() const noexcept {
        return _handle.id64();
    }

    /**
     * @brief Returns the stamped owner pointer cast to the requested type
     * @tparam OwnerT Expected owner class type (e.g., WorkContractGroup)
     * @return Owner pointer cast to OwnerT*, or nullptr if not stamped
     * @note Safe by construction: owners stamp the object with the true owner pointer
     */
    template <class OwnerT>
    OwnerT* handleOwnerAs() const noexcept {
        return static_cast<OwnerT*>(_handle.owner);
    }

    /** @brief Runtime class name for diagnostics and reflection */
    virtual const char* className() const noexcept {
        return "EntropyObject";
    }
    /** @brief Stable type hash for cross-language identification */
    virtual uint64_t classHash() const noexcept;

    /** @brief Human-readable short string (class@ptr by default) */
    virtual std::string toString() const;
    /** @brief Debug-oriented string including refcount and handle when present */
    virtual std::string debugString() const;
    /** @brief Long-form description; defaults to toString() */
    virtual std::string description() const;

    /** @brief Optional richer type information; may be null */
    virtual const TypeSystem::TypeInfo* typeInfo() const {
        return nullptr;
    }
};

/**
 * @brief Helper allowing owners to stamp/clear identity without friending each derived type
 *
 * Usage: HandleAccess::set(obj, owner, index, generation) when allocating; and
 * HandleAccess::clear(obj) before releasing/bumping generation.
 */
struct HandleAccess
{
    static void set(EntropyObject& o, void* owner, uint32_t index, uint32_t generation) noexcept {
        o._setHandleIdentity(owner, index, generation);
    }
    static void clear(EntropyObject& o) noexcept {
        o._clearHandleIdentity();
    }
};

}  // namespace EntropyEngine::Core
