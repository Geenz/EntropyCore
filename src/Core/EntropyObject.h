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
}
class EntropyObject {
protected:
    mutable std::atomic<uint32_t> _refCount{1};
    
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
    
    virtual const char* className() const noexcept { return "EntropyObject"; }
    virtual uint64_t classHash() const noexcept;
    
    virtual std::string toString() const;
    virtual std::string debugString() const;
    virtual std::string description() const;
    
    virtual const TypeSystem::TypeInfo* typeInfo() const { return nullptr; }
};


} // namespace EntropyEngine::Core