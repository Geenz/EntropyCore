/*
 * C API bridge for EntropyCore
 */

#include "entropy_c_api.h"
#include "EntropyObject.h"
#include "EntropyClass.h"
#include "../Logging/Logger.h"
#include <new>
#include <string>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <vector>

using namespace EntropyEngine::Core;

extern "C" {

ENTROPY_API void entropy_get_version(uint32_t* major, uint32_t* minor, uint32_t* patch, uint32_t* abi) {
    if (major) *major = 1;
    if (minor) *minor = 0;
    if (patch) *patch = 0;
    if (abi)   *abi   = ENTROPY_C_ABI_VERSION;
}

ENTROPY_API void* entropy_alloc(size_t size) {
    return ::operator new(size, std::nothrow);
}

ENTROPY_API void entropy_free(void* p) {
    ::operator delete(p);
}

static inline EntropyObject* to_cpp(EntropyObjectRef* o) {
    return reinterpret_cast<EntropyObject*>(o);
}
static inline const EntropyObject* to_cpp_c(const EntropyObjectRef* o) {
    return reinterpret_cast<const EntropyObject*>(o);
}

ENTROPY_API void entropy_object_retain(const EntropyObjectRef* obj) {
    if (obj) to_cpp_c(obj)->retain();
}

ENTROPY_API void entropy_object_release(const EntropyObjectRef* obj) {
    if (obj) to_cpp_c(obj)->release();
}

ENTROPY_API uint32_t entropy_object_ref_count(const EntropyObjectRef* obj) {
    return obj ? to_cpp_c(obj)->refCount() : 0;
}

ENTROPY_API EntropyTypeId entropy_object_type_id(const EntropyObjectRef* obj) {
    return obj ? to_cpp_c(obj)->classHash() : 0u;
}

ENTROPY_API const char* entropy_object_class_name(const EntropyObjectRef* obj) {
    return obj ? to_cpp_c(obj)->className() : "";
}

static EntropyStatus copy_string_out(const std::string& s, const char** out_str) {
    if (!out_str) return ENTROPY_ERR_INVALID_ARG;
    char* mem = static_cast<char*>(entropy_alloc(s.size() + 1));
    if (!mem) return ENTROPY_ERR_NO_MEMORY;
    std::memcpy(mem, s.c_str(), s.size() + 1);
    *out_str = mem;
    return ENTROPY_OK;
}

ENTROPY_API EntropyStatus entropy_object_to_string(const EntropyObjectRef* obj, const char** out_str) {
    if (!obj) return ENTROPY_ERR_INVALID_ARG;
    try {
        return copy_string_out(to_cpp_c(obj)->toString(), out_str);
    } catch (...) {
        return ENTROPY_ERR_UNKNOWN;
    }
}

ENTROPY_API EntropyStatus entropy_object_debug_string(const EntropyObjectRef* obj, const char** out_str) {
    if (!obj) return ENTROPY_ERR_INVALID_ARG;
    try {
        return copy_string_out(to_cpp_c(obj)->debugString(), out_str);
    } catch (...) {
        return ENTROPY_ERR_UNKNOWN;
    }
}

ENTROPY_API EntropyStatus entropy_object_description(const EntropyObjectRef* obj, const char** out_str) {
    if (!obj) return ENTROPY_ERR_INVALID_ARG;
    try {
        return copy_string_out(to_cpp_c(obj)->description(), out_str);
    } catch (...) {
        return ENTROPY_ERR_UNKNOWN;
    }
}

ENTROPY_API int entropy_handle_is_valid(EntropyHandle h) {
    return h.owner != nullptr; // quick check only
}

ENTROPY_API int entropy_handle_equals(EntropyHandle a, EntropyHandle b) {
    return (a.owner == b.owner) && (a.index == b.index) && (a.generation == b.generation) && (a.type == b.type);
}

ENTROPY_API int entropy_handle_type_matches(EntropyHandle h, EntropyTypeId expected) {
    // Strict matching: unknown type (0) never matches, and expecting 0 never matches
    return (expected != 0) && (h.type != 0) && (h.type == expected);
}

ENTROPY_API EntropyStatus entropy_object_to_handle(const EntropyObjectRef* obj, EntropyHandle* out_handle) {
    if (!obj || !out_handle) return ENTROPY_ERR_INVALID_ARG;
    if (!to_cpp_c(obj)->hasHandle()) return ENTROPY_ERR_UNAVAILABLE;
    EntropyHandle h{};
    h.owner = to_cpp_c(obj)->handleOwner();
    h.index = to_cpp_c(obj)->handleIndex();
    h.generation = to_cpp_c(obj)->handleGeneration();
    h.type = to_cpp_c(obj)->classHash();
    *out_handle = h;
    return ENTROPY_OK;
}

// Owner vtable registry -------------------------------------------------------
struct OwnerVTable { EntropyResolveFn resolve; EntropyValidateFn validate; };
static std::unordered_map<const void*, OwnerVTable> g_ownerVTables;
static std::mutex g_ownerVTablesMutex;

ENTROPY_API void entropy_register_owner_vtable(const void* owner, EntropyResolveFn resolve, EntropyValidateFn validate) {
    std::lock_guard<std::mutex> lock(g_ownerVTablesMutex);
    g_ownerVTables[owner] = OwnerVTable{ resolve, validate };
}

ENTROPY_API EntropyObjectRef* entropy_resolve_handle(EntropyHandle h) {
    if (!h.owner) return nullptr;
    std::lock_guard<std::mutex> lock(g_ownerVTablesMutex);
    auto it = g_ownerVTables.find(h.owner);
    if (it == g_ownerVTables.end()) return nullptr;
    auto fn = it->second.resolve;
    if (!fn) return nullptr;
    // Contract: resolve returns a RETAINED pointer if valid; otherwise NULL
    return fn(h.owner, h.index, h.generation);
}

// (No demo functionality is provided in the C API implementation. The API is production-only.)

} // extern "C"
