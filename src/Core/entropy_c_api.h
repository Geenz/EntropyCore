#pragma once

// C ABI for EntropyCore base object and handle interop
// This header is C-compatible and can be consumed by C, Rust, C#, etc.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI versioning
#define ENTROPY_C_ABI_VERSION 1u

// Export macro (works for both static and shared builds)
#if defined(_WIN32)
  #if defined(ENTROPYCORE_SHARED)
    #if defined(ENTROPYCORE_BUILDING)
      #define ENTROPY_API __declspec(dllexport)
    #else
      #define ENTROPY_API __declspec(dllimport)
    #endif
  #else
    #define ENTROPY_API
  #endif
#else
  #if defined(ENTROPYCORE_SHARED)
    #define ENTROPY_API __attribute__((visibility("default")))
  #else
    #define ENTROPY_API
  #endif
#endif

// Status codes for C API functions
typedef enum EntropyStatus {
    ENTROPY_OK = 0,
    ENTROPY_ERR_UNKNOWN = 1,
    ENTROPY_ERR_INVALID_ARG = 2,
    ENTROPY_ERR_NOT_FOUND = 3,
    ENTROPY_ERR_TYPE_MISMATCH = 4,
    ENTROPY_ERR_BUFFER_TOO_SMALL = 5,
    ENTROPY_ERR_NO_MEMORY = 6,
    ENTROPY_ERR_UNAVAILABLE = 7
} EntropyStatus;

// Opaque base object reference (C++ EntropyObject)
// Use a distinct tag name to avoid conflicting with C++ EntropyObject class
typedef struct EntropyObjectRefTag EntropyObjectRef;

// Type identifier (maps to EntropyObject::classHash())
typedef uint64_t EntropyTypeId;

// POD value handle (owner pointer is process-local)
typedef struct EntropyHandle {
    const void* owner;
    uint32_t    index;
    uint32_t    generation;
    EntropyTypeId type; // optional canonical type id (0 if unknown)
} EntropyHandle;

// Library/version/memory -----------------------------------------------------
ENTROPY_API void entropy_get_version(uint32_t* major, uint32_t* minor, uint32_t* patch, uint32_t* abi);
ENTROPY_API void* entropy_alloc(size_t size);
ENTROPY_API void  entropy_free(void* p);

// Lifetime management ---------------------------------------------------------
ENTROPY_API void     entropy_object_retain(const EntropyObjectRef* obj);
ENTROPY_API void     entropy_object_release(const EntropyObjectRef* obj);
ENTROPY_API uint32_t entropy_object_ref_count(const EntropyObjectRef* obj);

// Introspection ---------------------------------------------------------------
ENTROPY_API EntropyTypeId entropy_object_type_id(const EntropyObjectRef* obj);
ENTROPY_API const char*   entropy_object_class_name(const EntropyObjectRef* obj); // borrowed, do not free

// Returned strings are UTF-8 and must be freed with entropy_free
ENTROPY_API EntropyStatus entropy_object_to_string(const EntropyObjectRef* obj, const char** out_str);
ENTROPY_API EntropyStatus entropy_object_debug_string(const EntropyObjectRef* obj, const char** out_str);
ENTROPY_API EntropyStatus entropy_object_description(const EntropyObjectRef* obj, const char** out_str);

// Object <-> handle -----------------------------------------------------------
ENTROPY_API EntropyStatus entropy_object_to_handle(const EntropyObjectRef* obj, EntropyHandle* out_handle);
ENTROPY_API int           entropy_handle_is_valid(EntropyHandle h);     // 0/1
ENTROPY_API int           entropy_handle_equals(EntropyHandle a, EntropyHandle b);
ENTROPY_API int           entropy_handle_type_matches(EntropyHandle h, EntropyTypeId expected);

// Owner vtable registration & generic resolver (process-local) ---------------
typedef EntropyObjectRef* (*EntropyResolveFn)(const void* owner, uint32_t index, uint32_t generation);
typedef int                (*EntropyValidateFn)(const void* owner, uint32_t index, uint32_t generation);

ENTROPY_API void entropy_register_owner_vtable(const void* owner, EntropyResolveFn resolve, EntropyValidateFn validate);
ENTROPY_API EntropyObjectRef* entropy_resolve_handle(EntropyHandle h); // returns retained object or NULL


#ifdef __cplusplus
} // extern "C"
#endif
