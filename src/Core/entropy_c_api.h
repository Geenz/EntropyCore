#pragma once

// C ABI for EntropyCore base object and handle interop
// This header is C-compatible and can be consumed by C, Rust, C#, etc.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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

// Booleans (explicit, stable width across languages)
typedef int32_t EntropyBool; // 0 = false, non-zero = true
#define ENTROPY_FALSE 0
#define ENTROPY_TRUE  1

// Owned string (UTF-8). Caller must dispose via entropy_string_dispose.
typedef struct EntropyOwnedString {
    const char* ptr;
    uint32_t    len;
} EntropyOwnedString;

// Owned buffer (bytes). Caller must dispose via entropy_buffer_dispose.
typedef struct EntropyOwnedBuffer {
    uint8_t* ptr;
    uint32_t len;
} EntropyOwnedBuffer;

// Opaque base object reference (C++ EntropyObject)
// Use a distinct tag name to avoid conflicting with C++ EntropyObject class
typedef struct EntropyObjectRefTag EntropyObjectRef;

// Type identifier (maps to EntropyObject::classHash())
typedef uint64_t EntropyTypeId;

// POD value handle (owner pointer is process-local)
typedef struct EntropyHandle {
    const void*   owner;
    uint32_t      index;
    uint32_t      generation;
    EntropyTypeId type_id; // optional canonical type id (0 if unknown)
} EntropyHandle;

// Library/version/memory -----------------------------------------------------
ENTROPY_API void entropy_get_version(uint32_t* major, uint32_t* minor, uint32_t* patch, uint32_t* abi);
ENTROPY_API void* entropy_alloc(size_t size);
ENTROPY_API void  entropy_free(void* p);

// Convenience/diagnostics ----------------------------------------------------
ENTROPY_API const char* entropy_status_to_string(EntropyStatus s); // static string, no free
ENTROPY_API void        entropy_string_free(const char* s);        // alias of entropy_free
ENTROPY_API void        entropy_string_dispose(EntropyOwnedString s);
ENTROPY_API void        entropy_buffer_dispose(EntropyOwnedBuffer b);

// Lifetime management (pointer-based; native/internal) -----------------------
ENTROPY_API void     entropy_object_retain(const EntropyObjectRef* obj);
ENTROPY_API void     entropy_object_release(const EntropyObjectRef* obj);
ENTROPY_API uint32_t entropy_object_ref_count(const EntropyObjectRef* obj);

// Introspection (pointer-based) ----------------------------------------------
ENTROPY_API EntropyTypeId     entropy_object_type_id(const EntropyObjectRef* obj);
ENTROPY_API const char*       entropy_object_class_name(const EntropyObjectRef* obj); // borrowed, do not free
ENTROPY_API EntropyStatus     entropy_object_class_name_owned(const EntropyObjectRef* obj, EntropyOwnedString* out);
ENTROPY_API EntropyStatus     entropy_object_to_string(const EntropyObjectRef* obj, EntropyOwnedString* out);
ENTROPY_API EntropyStatus     entropy_object_debug_string(const EntropyObjectRef* obj, EntropyOwnedString* out);
ENTROPY_API EntropyStatus     entropy_object_description(const EntropyObjectRef* obj, EntropyOwnedString* out);

// Handle helpers --------------------------------------------------------------
ENTROPY_API EntropyStatus entropy_object_to_handle(const EntropyObjectRef* obj, EntropyHandle* out_handle);
ENTROPY_API EntropyBool   entropy_handle_is_valid(EntropyHandle h);
ENTROPY_API EntropyBool   entropy_handle_equals(EntropyHandle a, EntropyHandle b);
ENTROPY_API EntropyBool   entropy_handle_type_matches(EntropyHandle h, EntropyTypeId expected);

// Handle-first operations -----------------------------------------------------
// Adjust object lifetime using a value handle (in-process only)
ENTROPY_API EntropyStatus entropy_handle_retain(EntropyHandle h);
ENTROPY_API EntropyStatus entropy_handle_release(EntropyHandle h);

// Fetch basic info by handle (owned class name)
ENTROPY_API EntropyStatus entropy_handle_info(EntropyHandle h,
                                             EntropyTypeId* out_type_id,
                                             EntropyOwnedString* out_class_name);

// Generic call surface (opaque method ids and byte payloads)
ENTROPY_API EntropyStatus entropy_call(EntropyHandle h,
                                       uint32_t method_id,
                                       const uint8_t* req, uint32_t req_len,
                                       /*out*/ uint8_t** out_resp, /*out*/ uint32_t* out_resp_len);

// Convenience wrapper using owned buffer
ENTROPY_API EntropyStatus entropy_call_buf(EntropyHandle h,
                                           uint32_t method_id,
                                           const uint8_t* req, uint32_t req_len,
                                           /*out*/ EntropyOwnedBuffer* out_resp);

// Owner vtable registration & generic resolver (process-local) ---------------
typedef EntropyObjectRef* (*EntropyResolveFn)(const void* owner, uint32_t index, uint32_t generation);
typedef int                (*EntropyValidateFn)(const void* owner, uint32_t index, uint32_t generation);

ENTROPY_API void entropy_register_owner_vtable(const void* owner, EntropyResolveFn resolve, EntropyValidateFn validate);
ENTROPY_API EntropyObjectRef* entropy_resolve_handle(EntropyHandle h); // returns retained object or NULL

#ifdef __cplusplus
} // extern "C"
#endif
