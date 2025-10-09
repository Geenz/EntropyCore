#pragma once

/**
 * @file entropy_file_operation_handle.h
 * @brief C API for FileOperationHandle - async operation results
 *
 * FileOperationHandle represents an asynchronous file operation. Call wait()
 * to block until completion, then query results via status() and content getters.
 */

#include "entropy/entropy_vfs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * FileOperationHandle Lifecycle
 * ============================================================================ */

/**
 * @brief Clone a file operation handle
 *
 * Creates a copy that references the same underlying operation. Both handles
 * remain valid and must be destroyed independently.
 *
 * @param handle Handle to clone (required)
 * @param status Error reporting (required)
 * @return Owned clone or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_operation_handle_clone(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
);

/**
 * @brief Destroy a file operation handle
 *
 * Releases the handle. The underlying operation continues if other handles
 * reference it. Does not cancel the operation.
 *
 * @param handle Handle to destroy (can be NULL)
 * @threadsafety Thread-safe
 */
ENTROPY_API void entropy_file_operation_handle_destroy(
    entropy_FileOperationHandle handle
);

/* ============================================================================
 * Synchronization
 * ============================================================================ */

/**
 * @brief Wait for the operation to complete
 *
 * Blocks until the operation reaches a terminal state (Complete, Partial, Failed).
 * Safe to call multiple times; subsequent calls return immediately.
 *
 * @param handle Operation handle (required)
 * @param status Error reporting (required)
 * @threadsafety Thread-safe
 */
ENTROPY_API void entropy_file_operation_handle_wait(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
);

/**
 * @brief Get current operation status
 *
 * Returns the current state without blocking. Use after wait() to determine outcome.
 *
 * @param handle Operation handle (required)
 * @param status Error reporting (required)
 * @return Current status
 * @threadsafety Thread-safe
 */
ENTROPY_API EntropyFileOpStatus entropy_file_operation_handle_status(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
);

/* ============================================================================
 * Read Results - only valid after wait()
 * ============================================================================ */

/**
 * @brief Get read result as bytes
 *
 * Returns a pointer to the internal buffer and its size. The buffer is valid
 * until the handle is destroyed. Returns NULL if operation did not produce bytes.
 *
 * @param handle Operation handle (required)
 * @param out_size Receives byte count (required)
 * @param status Error reporting (required)
 * @return Borrowed pointer to bytes (valid until handle destroyed) or NULL
 * @threadsafety NOT thread-safe - do not call concurrently on same handle
 * @ownership Returns borrowed pointer - do NOT free
 *
 * @code
 * size_t size;
 * const uint8_t* bytes = entropy_file_operation_handle_contents_bytes(op, &size, &status);
 * if (bytes) {
 *     // Use bytes[0..size-1]
 * }
 * @endcode
 */
ENTROPY_API const uint8_t* entropy_file_operation_handle_contents_bytes(
    entropy_FileOperationHandle handle,
    size_t* out_size,
    EntropyStatus* status
);

/**
 * @brief Get read result as text
 *
 * Returns a pointer to the internal text buffer. The buffer is valid until
 * the handle is destroyed. Returns NULL if operation did not produce text.
 *
 * @param handle Operation handle (required)
 * @param status Error reporting (required)
 * @return Borrowed pointer to null-terminated UTF-8 text or NULL
 * @threadsafety NOT thread-safe - do not call concurrently on same handle
 * @ownership Returns borrowed pointer - do NOT free
 */
ENTROPY_API const char* entropy_file_operation_handle_contents_text(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
);

/* ============================================================================
 * Write Results - only valid after wait()
 * ============================================================================ */

/**
 * @brief Get number of bytes written
 *
 * Returns the actual byte count written for write operations.
 *
 * @param handle Operation handle (required)
 * @param status Error reporting (required)
 * @return Bytes written (0 if not a write operation)
 * @threadsafety Thread-safe
 */
ENTROPY_API uint64_t entropy_file_operation_handle_bytes_written(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
);

/* ============================================================================
 * Metadata Results - only valid after wait()
 * ============================================================================ */

/**
 * @brief Get file metadata result
 *
 * Returns metadata for operations that query file properties. The returned
 * pointer is valid until the handle is destroyed.
 *
 * @param handle Operation handle (required)
 * @param status Error reporting (required)
 * @return Borrowed pointer to metadata or NULL if unavailable
 * @threadsafety NOT thread-safe - do not call concurrently on same handle
 * @ownership Returns borrowed pointer - do NOT free
 */
ENTROPY_API const EntropyFileMetadata* entropy_file_operation_handle_metadata(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
);

/* ============================================================================
 * Directory Listing Results - only valid after wait()
 * ============================================================================ */

/**
 * @brief Get directory listing results
 *
 * Returns an array of directory entries. The array and all pointers within
 * are valid until the handle is destroyed.
 *
 * @param handle Operation handle (required)
 * @param out_count Receives entry count (required)
 * @param status Error reporting (required)
 * @return Borrowed pointer to entry array or NULL
 * @threadsafety NOT thread-safe - do not call concurrently on same handle
 * @ownership Returns borrowed pointer - do NOT free
 *
 * @code
 * size_t count;
 * const EntropyDirectoryEntry* entries =
 *     entropy_file_operation_handle_directory_entries(op, &count, &status);
 * for (size_t i = 0; i < count; i++) {
 *     printf("%s\n", entries[i].name);
 * }
 * @endcode
 */
ENTROPY_API const EntropyDirectoryEntry* entropy_file_operation_handle_directory_entries(
    entropy_FileOperationHandle handle,
    size_t* out_count,
    EntropyStatus* status
);

/* ============================================================================
 * Error Information - only valid after wait() when status is Failed
 * ============================================================================ */

/**
 * @brief Get error information for failed operations
 *
 * Returns detailed error info if status is ENTROPY_FILE_OP_FAILED. The returned
 * pointer and all strings within are valid until the handle is destroyed.
 *
 * @param handle Operation handle (required)
 * @param status Error reporting (required)
 * @return Borrowed pointer to error info or NULL if no error
 * @threadsafety NOT thread-safe - do not call concurrently on same handle
 * @ownership Returns borrowed pointer - do NOT free
 *
 * @code
 * if (entropy_file_operation_handle_status(op, &status) == ENTROPY_FILE_OP_FAILED) {
 *     const EntropyFileErrorInfo* err =
 *         entropy_file_operation_handle_error_info(op, &status);
 *     printf("Error: %s\n", err->message);
 * }
 * @endcode
 */
ENTROPY_API const EntropyFileErrorInfo* entropy_file_operation_handle_error_info(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
);

#ifdef __cplusplus
} // extern "C"
#endif
