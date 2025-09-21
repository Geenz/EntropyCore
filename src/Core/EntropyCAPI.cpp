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
    if (abi)   *abi   = 0; // versioning not a concern right now
}

ENTROPY_API void* entropy_alloc(size_t size) {
    return ::operator new(size, std::nothrow);
}

ENTROPY_API void entropy_free(void* p) {
    ::operator delete(p);
}

ENTROPY_API const char* entropy_status_to_string(EntropyStatus s) {
    switch (s) {
        case ENTROPY_OK: return "ENTROPY_OK";
        case ENTROPY_ERR_UNKNOWN: return "ENTROPY_ERR_UNKNOWN";
        case ENTROPY_ERR_INVALID_ARG: return "ENTROPY_ERR_INVALID_ARG";
        case ENTROPY_ERR_NOT_FOUND: return "ENTROPY_ERR_NOT_FOUND";
        case ENTROPY_ERR_TYPE_MISMATCH: return "ENTROPY_ERR_TYPE_MISMATCH";
        case ENTROPY_ERR_BUFFER_TOO_SMALL: return "ENTROPY_ERR_BUFFER_TOO_SMALL";
        case ENTROPY_ERR_NO_MEMORY: return "ENTROPY_ERR_NO_MEMORY";
        case ENTROPY_ERR_UNAVAILABLE: return "ENTROPY_ERR_UNAVAILABLE";
        default: return "ENTROPY_STATUS_UNKNOWN";
    }
}

ENTROPY_API void entropy_string_free(const char* s) {
    entropy_free((void*)s);
}

ENTROPY_API void entropy_string_dispose(EntropyOwnedString s) {
    if (s.ptr) entropy_free((void*)s.ptr);
}

ENTROPY_API void entropy_buffer_dispose(EntropyOwnedBuffer b) {
    if (b.ptr) entropy_free((void*)b.ptr);
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

static EntropyStatus copy_string_out(const std::string& s, EntropyOwnedString* out) {
    if (!out) return ENTROPY_ERR_INVALID_ARG;
    char* mem = static_cast<char*>(entropy_alloc(s.size()));
    if (!mem && s.size() != 0) return ENTROPY_ERR_NO_MEMORY;
    if (s.size()) std::memcpy(mem, s.data(), s.size());
    out->ptr = mem;
    out->len = static_cast<uint32_t>(s.size());
    return ENTROPY_OK;
}

ENTROPY_API EntropyStatus entropy_object_class_name_owned(const EntropyObjectRef* obj, EntropyOwnedString* out) {
    if (!obj) return ENTROPY_ERR_INVALID_ARG;
    try {
        return copy_string_out(std::string(to_cpp_c(obj)->className()), out);
    } catch (...) {
        return ENTROPY_ERR_UNKNOWN;
    }
}

ENTROPY_API EntropyStatus entropy_object_to_string(const EntropyObjectRef* obj, EntropyOwnedString* out) {
    if (!obj) return ENTROPY_ERR_INVALID_ARG;
    try {
        return copy_string_out(to_cpp_c(obj)->toString(), out);
    } catch (...) {
        return ENTROPY_ERR_UNKNOWN;
    }
}

ENTROPY_API EntropyStatus entropy_object_debug_string(const EntropyObjectRef* obj, EntropyOwnedString* out) {
    if (!obj) return ENTROPY_ERR_INVALID_ARG;
    try {
        return copy_string_out(to_cpp_c(obj)->debugString(), out);
    } catch (...) {
        return ENTROPY_ERR_UNKNOWN;
    }
}

ENTROPY_API EntropyStatus entropy_object_description(const EntropyObjectRef* obj, EntropyOwnedString* out) {
    if (!obj) return ENTROPY_ERR_INVALID_ARG;
    try {
        return copy_string_out(to_cpp_c(obj)->description(), out);
    } catch (...) {
        return ENTROPY_ERR_UNKNOWN;
    }
}

ENTROPY_API EntropyBool entropy_handle_is_valid(EntropyHandle h) {
    return h.owner != nullptr ? ENTROPY_TRUE : ENTROPY_FALSE; // quick check only
}

ENTROPY_API EntropyBool entropy_handle_equals(EntropyHandle a, EntropyHandle b) {
    return ((a.owner == b.owner) && (a.index == b.index) && (a.generation == b.generation) && (a.type_id == b.type_id)) ? ENTROPY_TRUE : ENTROPY_FALSE;
}

ENTROPY_API EntropyBool entropy_handle_type_matches(EntropyHandle h, EntropyTypeId expected) {
    // Strict matching: unknown type (0) never matches, and expecting 0 never matches
    return ((expected != 0) && (h.type_id != 0) && (h.type_id == expected)) ? ENTROPY_TRUE : ENTROPY_FALSE;
}

ENTROPY_API EntropyStatus entropy_object_to_handle(const EntropyObjectRef* obj, EntropyHandle* out_handle) {
    if (!obj || !out_handle) return ENTROPY_ERR_INVALID_ARG;
    if (!to_cpp_c(obj)->hasHandle()) return ENTROPY_ERR_UNAVAILABLE;
    EntropyHandle h{};
    h.owner = to_cpp_c(obj)->handleOwner();
    h.index = to_cpp_c(obj)->handleIndex();
    h.generation = to_cpp_c(obj)->handleGeneration();
    h.type_id = to_cpp_c(obj)->classHash();
    *out_handle = h;
    return ENTROPY_OK;
}

// Handle-first operations -----------------------------------------------------
ENTROPY_API EntropyStatus entropy_handle_retain(EntropyHandle h) {
    if (!h.owner) return ENTROPY_ERR_INVALID_ARG;
    EntropyObjectRef* obj = entropy_resolve_handle(h);
    if (!obj) return ENTROPY_ERR_NOT_FOUND;
    // resolve returns retained (+1). That is exactly the net +1 we need.
    // Do NOT retain/release again; just drop the local pointer without releasing
    // to keep the incremented refcount.
    return ENTROPY_OK;
}

ENTROPY_API EntropyStatus entropy_handle_release(EntropyHandle h) {
    if (!h.owner) return ENTROPY_ERR_INVALID_ARG;
    EntropyObjectRef* obj = entropy_resolve_handle(h);
    if (!obj) return ENTROPY_ERR_NOT_FOUND;
    // Drop the resolved ref and one more to achieve net -1
    entropy_object_release(obj); // back to 0 net
    entropy_object_release(obj); // net -1
    return ENTROPY_OK;
}

ENTROPY_API EntropyStatus entropy_handle_info(EntropyHandle h,
                                             EntropyTypeId* out_type_id,
                                             EntropyOwnedString* out_class_name) {
    if (!h.owner) return ENTROPY_ERR_INVALID_ARG;
    EntropyObjectRef* obj = entropy_resolve_handle(h);
    if (!obj) return ENTROPY_ERR_NOT_FOUND;
    EntropyStatus st = ENTROPY_OK;
    if (out_type_id) *out_type_id = entropy_object_type_id(obj);
    if (out_class_name) {
        st = entropy_object_class_name_owned(obj, out_class_name);
    }
    entropy_object_release(obj);
    return st;
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
