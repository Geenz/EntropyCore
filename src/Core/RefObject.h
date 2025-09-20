//
// Created by goodh on 9/17/2025.
//

#pragma once
#include "EntropyObject.h"
#include <utility>
#include <type_traits>
#include <functional>

namespace EntropyEngine::Core {

struct adopt_t { explicit adopt_t() = default; };
struct retain_t { explicit retain_t() = default; };
inline constexpr adopt_t adopt{};
inline constexpr retain_t retain{};

template<typename T>
class RefObject {
    static_assert(std::is_base_of_v<EntropyObject, T>, 
                  "T must derive from EntropyObject");
    
    T* _ptr = nullptr;
    
public:
    RefObject() noexcept = default;
    
    // Adopting constructor (default behavior)
    explicit RefObject(T* ptr) noexcept : _ptr(ptr) {}

    // Explicit tag-based constructors to clarify intent
    explicit RefObject(adopt_t, T* ptr) noexcept : _ptr(ptr) {}
    explicit RefObject(retain_t, T* ptr) noexcept : _ptr(ptr) { if (_ptr) _ptr->retain(); }
            
    ~RefObject() noexcept {
        if (_ptr) _ptr->release();
    }
    
    RefObject(RefObject&& other) noexcept 
        : _ptr(std::exchange(other._ptr, nullptr)) {}
        
    RefObject& operator=(RefObject&& other) noexcept {
        if (this != &other) {
            if (_ptr) _ptr->release();
            _ptr = std::exchange(other._ptr, nullptr);
        }
        return *this;
    }
    
    RefObject(const RefObject& other) noexcept : _ptr(other._ptr) {
        if (_ptr) _ptr->retain();
    }
    
    RefObject& operator=(const RefObject& other) noexcept {
        if (this != &other) {
            T* old = _ptr;
            _ptr = other._ptr;
            if (_ptr) _ptr->retain();
            if (old) old->release();
        }
        return *this;
    }

    // Converting copy ctor from RefObject<U> where U derives from T (upcast)
    template<class U, class = std::enable_if_t<std::is_base_of_v<T, U>>> 
    RefObject(const RefObject<U>& other) noexcept : _ptr(static_cast<T*>(other.get())) {
        if (_ptr) _ptr->retain();
    }

    // Converting move ctor from RefObject<U> where U derives from T (upcast)
    template<class U, class = std::enable_if_t<std::is_base_of_v<T, U>>> 
    RefObject(RefObject<U>&& other) noexcept : _ptr(static_cast<T*>(other.detach())) {}

    // Converting copy assignment
    template<class U, class = std::enable_if_t<std::is_base_of_v<T, U>>> 
    RefObject& operator=(const RefObject<U>& other) noexcept {
        T* newPtr = static_cast<T*>(other.get());
        if (_ptr != newPtr) {
            if (newPtr) newPtr->retain();
            T* old = _ptr;
            _ptr = newPtr;
            if (old) old->release();
        }
        return *this;
    }

    // Converting move assignment
    template<class U, class = std::enable_if_t<std::is_base_of_v<T, U>>> 
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
    
    T* get() const noexcept { return _ptr; }
    T* operator->() const noexcept { return _ptr; }
    T& operator*() const noexcept { return *_ptr; }
    explicit operator bool() const noexcept { return _ptr != nullptr; }
    
    [[nodiscard]] T* detach() noexcept {
        return std::exchange(_ptr, nullptr);
    }
    
    void reset(T* ptr = nullptr) noexcept {
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

template<typename T, typename... Args>
[[nodiscard]] RefObject<T> makeRef(Args&&... args) {
    return RefObject<T>(new T(std::forward<Args>(args)...));
}

// Transparent hasher/equality for heterogeneous lookup by raw pointer
template<class T>
struct RefPtrHash {
    using is_transparent = void;
    size_t operator()(const RefObject<T>& r) const noexcept { return std::hash<const T*>{}(r.get()); }
    size_t operator()(const T* p) const noexcept { return std::hash<const T*>{}(p); }
};

template<class T>
struct RefPtrEq {
    using is_transparent = void;
    bool operator()(const RefObject<T>& a, const RefObject<T>& b) const noexcept { return a.get() == b.get(); }
    bool operator()(const RefObject<T>& a, const T* b) const noexcept { return a.get() == b; }
    bool operator()(const T* a, const RefObject<T>& b) const noexcept { return a == b.get(); }
};

// Casting helpers: wrap the same underlying pointer and RETAIN it to avoid double-release
template<class To, class From>
RefObject<To> ref_static_cast(const RefObject<From>& r) noexcept {
    auto p = static_cast<To*>(r.get());
    return p ? RefObject<To>(retain, p) : RefObject<To>{};
}

template<class To, class From>
RefObject<To> ref_dynamic_cast(const RefObject<From>& r) noexcept {
    if (auto p = dynamic_cast<To*>(r.get())) return RefObject<To>(retain, p);
    return RefObject<To>{};
}

} // namespace EntropyEngine::Core

// Hash support for RefObject<T> by identity
namespace std {
    template<class T>
    struct hash<EntropyEngine::Core::RefObject<T>> {
        size_t operator()(const EntropyEngine::Core::RefObject<T>& r) const noexcept {
            return std::hash<const T*>{}(r.get());
        }
    };
}