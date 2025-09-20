//
// Created by goodh on 9/17/2025.
//

#pragma once
#include <atomic>
#include <string>
#include <cstdint>
#include <string_view>
#include <functional>

namespace EntropyEngine::Core {

// Forward declarations
namespace TypeSystem {
    class TypeInfo;
    template<class OwnerType> class GenericHandle; // fwd decl for helpers (optional)
    template<class T, class OwnerType> class TypedHandle; // fwd decl for helpers (optional)
}
class EntropyObject {
protected:
    mutable std::atomic<uint32_t> _refCount{1};

    // Optional handle identity stamped by an owner/registry
    struct HandleCore {
        void* owner = nullptr;
        uint32_t index = 0;
        uint32_t generation = 0;
        bool isSet() const noexcept { return owner != nullptr; }
        uint64_t id64() const noexcept { return (static_cast<uint64_t>(index) << 32) | generation; }
    } _handle{};

    // Internal setters used by owners/registries to stamp identity
    void _setHandleIdentity(void* owner, uint32_t index, uint32_t generation) noexcept {
        _handle.owner = owner; _handle.index = index; _handle.generation = generation;
    }
    void _clearHandleIdentity() noexcept { _handle = {}; }

    // Grant access to helper
    friend struct HandleAccess;

public:
    EntropyObject() noexcept = default;
    EntropyObject(EntropyObject&&) = delete;
    EntropyObject(const EntropyObject&) = delete;
    EntropyObject& operator=(const EntropyObject&) = delete;
    EntropyObject& operator=(EntropyObject&&) = delete;
    
    virtual ~EntropyObject() noexcept = default;
    
    void retain() const noexcept;
    void release() const noexcept;
    uint32_t refCount() const noexcept;

    // Handle introspection (safe even if not stamped)
    bool hasHandle() const noexcept { return _handle.isSet(); }
    const void* handleOwner() const noexcept { return _handle.owner; }
    uint32_t handleIndex() const noexcept { return _handle.index; }
    uint32_t handleGeneration() const noexcept { return _handle.generation; }
    uint64_t handleId() const noexcept { return _handle.id64(); }
    
    virtual const char* className() const noexcept { return "EntropyObject"; }
    virtual uint64_t classHash() const noexcept;
    
    virtual std::string toString() const;
    virtual std::string debugString() const;
    virtual std::string description() const;
    
    virtual const TypeSystem::TypeInfo* typeInfo() const { return nullptr; }
};

// Minimal helper to allow owners to stamp/clear identity without friending
struct HandleAccess {
    static void set(EntropyObject& o, void* owner, uint32_t index, uint32_t generation) noexcept { o._setHandleIdentity(owner, index, generation); }
    static void clear(EntropyObject& o) noexcept { o._clearHandleIdentity(); }
};

} // namespace EntropyEngine::Core