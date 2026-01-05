//
// Created by goodh on 9/17/2025.
//

#pragma once
#include <functional>
#include <type_traits>
#include <utility>

#include "EntropyObject.h"
#include "WeakControlBlock.h"

namespace EntropyEngine::Core
{

struct adopt_t
{
    explicit adopt_t() = default;
};
struct retain_t
{
    explicit retain_t() = default;
};
inline constexpr adopt_t adopt{};
inline constexpr retain_t retain{};

template <typename T>
class RefObject
{
    static_assert(std::is_base_of_v<EntropyObject, T>, "T must derive from EntropyObject");

    T* _ptr = nullptr;

public:
    RefObject() noexcept = default;

    // Adopting constructor (default behavior)
    explicit RefObject(T* ptr) noexcept : _ptr(ptr) {}

    // Explicit tag-based constructors to clarify intent
    explicit RefObject(adopt_t, T* ptr) noexcept : _ptr(ptr) {}
    explicit RefObject(retain_t, T* ptr) noexcept : _ptr(nullptr) {
        if (ptr && ptr->tryRetain()) _ptr = ptr;
    }

    ~RefObject() noexcept {
        if (_ptr) _ptr->release();
    }

    RefObject(RefObject&& other) noexcept : _ptr(std::exchange(other._ptr, nullptr)) {}

    RefObject& operator=(RefObject&& other) noexcept {
        if (this != &other) {
            if (_ptr) _ptr->release();
            _ptr = std::exchange(other._ptr, nullptr);
        }
        return *this;
    }

    RefObject(const RefObject& other) noexcept : _ptr(other._ptr) {
        if (_ptr && !_ptr->tryRetain()) {
            _ptr = nullptr;
        }
    }

    RefObject& operator=(const RefObject& other) noexcept {
        if (this != &other) {
            T* newPtr = other._ptr;
            if (newPtr && !newPtr->tryRetain()) {
                newPtr = nullptr;
            }
            T* old = _ptr;
            _ptr = newPtr;
            if (old) old->release();
        }
        return *this;
    }

    // Converting copy ctor from RefObject<U> where U derives from T (upcast)
    template <class U, class = std::enable_if_t<std::is_base_of_v<T, U>>>
    RefObject(const RefObject<U>& other) noexcept : _ptr(static_cast<T*>(other.get())) {
        if (_ptr && !_ptr->tryRetain()) {
            _ptr = nullptr;
        }
    }

    // Converting move ctor from RefObject<U> where U derives from T (upcast)
    template <class U, class = std::enable_if_t<std::is_base_of_v<T, U>>>
    RefObject(RefObject<U>&& other) noexcept : _ptr(static_cast<T*>(other.detach())) {}

    // Converting copy assignment
    template <class U, class = std::enable_if_t<std::is_base_of_v<T, U>>>
    RefObject& operator=(const RefObject<U>& other) noexcept {
        T* newPtr = static_cast<T*>(other.get());
        if (_ptr != newPtr) {
            if (newPtr && !newPtr->tryRetain()) {
                newPtr = nullptr;
            }
            T* old = _ptr;
            _ptr = newPtr;
            if (old) old->release();
        }
        return *this;
    }

    // Converting move assignment
    template <class U, class = std::enable_if_t<std::is_base_of_v<T, U>>>
    RefObject& operator=(RefObject<U>&& other) noexcept {
        if (reinterpret_cast<void*>(_ptr) != reinterpret_cast<void*>(other.get())) {
            if (_ptr) _ptr->release();
            _ptr = static_cast<T*>(other.detach());
        } else {
            // Same underlying pointer, just detach source
            (void)other.detach();
        }
        return *this;
    }

    T* get() const noexcept {
        return _ptr;
    }
    T* operator->() const noexcept {
        return _ptr;
    }
    T& operator*() const noexcept {
        return *_ptr;
    }
    explicit operator bool() const noexcept {
        return _ptr != nullptr;
    }

    [[nodiscard]] T* detach() noexcept {
        return std::exchange(_ptr, nullptr);
    }

    void reset(T* ptr = nullptr) noexcept {
        if (ptr == _ptr) return;  // no-op if identical
        if (ptr) ptr->retain();   // take ownership of new before dropping old
        T* old = _ptr;
        _ptr = ptr;
        if (old) old->release();
    }

    friend void swap(RefObject& a, RefObject& b) noexcept {
        using std::swap;
        swap(a._ptr, b._ptr);
    }

    friend bool operator==(const RefObject& a, const RefObject& b) noexcept {
        return a._ptr == b._ptr;
    }
    friend bool operator!=(const RefObject& a, const RefObject& b) noexcept {
        return !(a == b);
    }
    friend bool operator<(const RefObject& a, const RefObject& b) noexcept {
        return std::less<const T*>{}(a._ptr, b._ptr);
    }
};

/**
 * @brief Non-owning weak reference to an EntropyObject with generation validation
 *
 * WeakRef stores a pointer and the object's handle generation at construction time.
 * Use lock() to safely acquire a strong RefObject reference - returns empty if the
 * object has been destroyed or if the slot has been reused for a different object.
 *
 * Generation validation prevents the memory-reuse problem: if the original object
 * is destroyed and a new object is allocated at the same address, lock() will
 * detect the generation mismatch and return empty instead of the wrong object.
 *
 * @note For full protection, the referenced object should be handle-stamped by
 * a service/pool using HandleSlotOps::stamp(). Non-stamped objects (generation 0)
 * fall back to refcount-only validation.
 *
 * @code
 * RefObject<Mesh> mesh = meshService->createMesh();  // Stamped with generation
 * WeakRef<Mesh> weak = mesh;
 *
 * // Later, safely try to use:
 * if (auto locked = weak.lock()) {
 *     locked->render();  // Safe - validated by generation + refcount
 * } // else: mesh was destroyed or slot reused
 * @endcode
 */
/**
 * @brief Weak reference to an EntropyObject, safely guarded by a control block
 */
template <typename T>
class WeakRef
{
    static_assert(std::is_base_of_v<EntropyObject, T>, "T must derive from EntropyObject");

    WeakControlBlock* _block = nullptr;

public:
    WeakRef() noexcept = default;

    /// Construct from raw pointer (acquires weak block)
    explicit WeakRef(T* ptr) noexcept {
        if (ptr) {
            _block = ptr->getWeakControlBlock();
            _block->retain();
        }
    }

    /// Construct from RefObject
    WeakRef(const RefObject<T>& ref) noexcept : WeakRef(ref.get()) {}

    /// Construct from derived RefObject
    template <class U, class = std::enable_if_t<std::is_base_of_v<T, U>>>
    WeakRef(const RefObject<U>& ref) noexcept : WeakRef(static_cast<T*>(ref.get())) {}

    ~WeakRef() noexcept {
        reset();
    }

    WeakRef(const WeakRef& other) noexcept : _block(other._block) {
        if (_block) _block->retain();
    }

    WeakRef& operator=(const WeakRef& other) noexcept {
        if (this != &other) {
            if (other._block) other._block->retain();
            reset();
            _block = other._block;
        }
        return *this;
    }

    WeakRef(WeakRef&& other) noexcept : _block(std::exchange(other._block, nullptr)) {}

    WeakRef& operator=(WeakRef&& other) noexcept {
        if (this != &other) {
            reset();
            _block = std::exchange(other._block, nullptr);
        }
        return *this;
    }

    /// Assign from RefObject
    WeakRef& operator=(const RefObject<T>& ref) noexcept {
        reset();
        if (T* ptr = ref.get()) {
            _block = ptr->getWeakControlBlock();
            _block->retain();
        }
        return *this;
    }

    /// Assign from derived RefObject
    template <class U, class = std::enable_if_t<std::is_base_of_v<T, U>>>
    WeakRef& operator=(const RefObject<U>& ref) noexcept {
        return operator=(RefObject<T>(ref));
    }

    /**
     * @brief Attempt to acquire a strong reference
     *
     * Safely checks if the object is still alive using the control block.
     * @return RefObject<T> if successful, empty RefObject if expired
     */
    [[nodiscard]] RefObject<T> lock() const noexcept {
        if (!_block) return {};

        std::lock_guard<std::mutex> lock(_block->mutex);
        if (auto* ptr = static_cast<T*>(_block->object)) {
            // Object is alive and we hold the lock, so it can't die while we tryRetain
            if (ptr->tryRetain()) {
                return RefObject<T>(adopt, ptr);
            }
        }
        return {};
    }

    [[nodiscard]] bool expired() const noexcept {
        if (!_block) return true;
        std::lock_guard<std::mutex> lock(_block->mutex);
        return _block->object == nullptr || _block->object->refCount() == 0;
    }

    void reset() noexcept {
        if (_block) {
            _block->release();
            _block = nullptr;
        }
    }

    friend bool operator==(const WeakRef& a, const WeakRef& b) noexcept {
        return a._block == b._block;
    }
    friend bool operator!=(const WeakRef& a, const WeakRef& b) noexcept {
        return !(a == b);
    }
};

template <typename T, typename... Args>
[[nodiscard]] RefObject<T> makeRef(Args&&... args) {
    T* ptr = new T(std::forward<Args>(args)...);
    // Call memory profiling hook after allocation
    if (EntropyObjectMemoryHooks::onAlloc) {
        EntropyObjectMemoryHooks::onAlloc(ptr, sizeof(T), "EntropyObject");
    }
    return RefObject<T>(ptr);
}

// Transparent hasher/equality for heterogeneous lookup by raw pointer
template <class T>
struct RefPtrHash
{
    using is_transparent = void;
    size_t operator()(const RefObject<T>& r) const noexcept {
        return std::hash<const T*>{}(r.get());
    }
    size_t operator()(const T* p) const noexcept {
        return std::hash<const T*>{}(p);
    }
};

template <class T>
struct RefPtrEq
{
    using is_transparent = void;
    bool operator()(const RefObject<T>& a, const RefObject<T>& b) const noexcept {
        return a.get() == b.get();
    }
    bool operator()(const RefObject<T>& a, const T* b) const noexcept {
        return a.get() == b;
    }
    bool operator()(const T* a, const RefObject<T>& b) const noexcept {
        return a == b.get();
    }
};

// Casting helpers: wrap the same underlying pointer and RETAIN it to avoid double-release
template <class To, class From>
RefObject<To> ref_static_cast(const RefObject<From>& r) noexcept {
    if (auto p = static_cast<To*>(r.get())) {
        if (p->tryRetain()) return RefObject<To>(adopt, p);
    }
    return RefObject<To>{};
}

template <class To, class From>
RefObject<To> ref_dynamic_cast(const RefObject<From>& r) noexcept {
    if (auto p = dynamic_cast<To*>(r.get())) {
        if (p->tryRetain()) return RefObject<To>(adopt, p);
    }
    return RefObject<To>{};
}

}  // namespace EntropyEngine::Core

// Hash support for RefObject<T> by identity
namespace std
{
template <class T>
struct hash<EntropyEngine::Core::RefObject<T>>
{
    size_t operator()(const EntropyEngine::Core::RefObject<T>& r) const noexcept {
        return std::hash<const T*>{}(r.get());
    }
};
}  // namespace std
