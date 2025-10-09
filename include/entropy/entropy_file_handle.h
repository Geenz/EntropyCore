#pragma once

/**
 * @file entropy_file_handle.h
 * @brief C API for FileHandle - value-semantic file operations
 *
 * FileHandle provides a copyable reference to a file path routed through a
 * VirtualFileSystem backend. All operations are asynchronous; call wait() on
 * the returned FileOperationHandle to block until completion.
 */

#include "entropy/entropy_vfs_types.h"
#include "entropy/entropy_file_operation_handle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * FileHandle Lifecycle
 * ============================================================================ */

/**
 * @brief Clone a file handle
 *
 * Creates a copy that references the same file path and VFS. Both handles
 * remain valid and must be destroyed independently.
 *
 * @param handle Handle to clone (required)
 * @param status Error reporting (required)
 * @return Owned clone or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_handle_destroy()
 */
ENTROPY_API entropy_FileHandle entropy_file_handle_clone(
    entropy_FileHandle handle,
    EntropyStatus* status
);

/**
 * @brief Destroy a file handle
 *
 * Releases the handle. Does not affect the underlying file or any pending operations.
 *
 * @param handle Handle to destroy (can be NULL)
 * @threadsafety Thread-safe
 */
ENTROPY_API void entropy_file_handle_destroy(entropy_FileHandle handle);

/* ============================================================================
 * Read Operations
 * ============================================================================ */

/**
 * @brief Read the entire file into memory
 *
 * Asynchronously reads the full contents. Call entropy_file_operation_handle_contents_bytes()
 * or entropy_file_operation_handle_contents_text() after wait().
 *
 * @param handle File handle (required)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 *
 * @code
 * entropy_FileOperationHandle op = entropy_file_handle_read_all(fh, &status);
 * entropy_file_operation_handle_wait(op, &status);
 * if (entropy_file_operation_handle_status(op, &status) == ENTROPY_FILE_OP_COMPLETE) {
 *     const char* text = entropy_file_operation_handle_contents_text(op, &status);
 *     printf("%s\n", text);
 * }
 * entropy_file_operation_handle_destroy(op);
 * @endcode
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_read_all(
    entropy_FileHandle handle,
    EntropyStatus* status
);

/**
 * @brief Read a byte range from the file
 *
 * @param handle File handle (required)
 * @param offset Starting byte offset
 * @param length Number of bytes to read
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_read_range(
    entropy_FileHandle handle,
    uint64_t offset,
    size_t length,
    EntropyStatus* status
);

/**
 * @brief Read a line by index (0-based)
 *
 * Reads the specified line, trimming line endings. Returns Partial if line index
 * is beyond EOF.
 *
 * @param handle File handle (required)
 * @param line_number Line index to read (0-based)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_read_line(
    entropy_FileHandle handle,
    size_t line_number,
    EntropyStatus* status
);

/* ============================================================================
 * Write Operations
 * ============================================================================ */

/**
 * @brief Write full text to the file
 *
 * Writes UTF-8 text using default options (overwrites by default).
 *
 * @param handle File handle (required)
 * @param text UTF-8 text to write (required)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_write_all_text(
    entropy_FileHandle handle,
    const char* text,
    EntropyStatus* status
);

/**
 * @brief Write full text to the file with options
 *
 * @param handle File handle (required)
 * @param text UTF-8 text to write (required)
 * @param options Write options (required, copied)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_write_all_text_with_options(
    entropy_FileHandle handle,
    const char* text,
    const EntropyWriteOptions* options,
    EntropyStatus* status
);

/**
 * @brief Write raw bytes to the file
 *
 * Writes binary data using default options (overwrites by default).
 *
 * @param handle File handle (required)
 * @param bytes Data to write (required)
 * @param length Number of bytes to write
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_write_all_bytes(
    entropy_FileHandle handle,
    const uint8_t* bytes,
    size_t length,
    EntropyStatus* status
);

/**
 * @brief Write raw bytes to the file with options
 *
 * @param handle File handle (required)
 * @param bytes Data to write (required)
 * @param length Number of bytes to write
 * @param options Write options (required, copied)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_write_all_bytes_with_options(
    entropy_FileHandle handle,
    const uint8_t* bytes,
    size_t length,
    const EntropyWriteOptions* options,
    EntropyStatus* status
);

/**
 * @brief Write bytes starting at a specific offset
 *
 * @param handle File handle (required)
 * @param offset Byte offset to begin writing
 * @param bytes Data to write (required)
 * @param length Number of bytes to write
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_write_range(
    entropy_FileHandle handle,
    uint64_t offset,
    const uint8_t* bytes,
    size_t length,
    EntropyStatus* status
);

/**
 * @brief Replace a single line by index (0-based)
 *
 * Overwrites the specified line. Extends the file with blank lines if the
 * index is beyond EOF.
 *
 * @param handle File handle (required)
 * @param line_number Line index to overwrite (0-based)
 * @param line New line content (required, without newline)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_write_line(
    entropy_FileHandle handle,
    size_t line_number,
    const char* line,
    EntropyStatus* status
);

/* ============================================================================
 * File Management
 * ============================================================================ */

/**
 * @brief Create an empty file or truncate to zero
 *
 * @param handle File handle (required)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_create_empty(
    entropy_FileHandle handle,
    EntropyStatus* status
);

/**
 * @brief Delete the file if it exists (idempotent)
 *
 * @param handle File handle (required)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_file_handle_remove(
    entropy_FileHandle handle,
    EntropyStatus* status
);

/* ============================================================================
 * Metadata Access
 * ============================================================================ */

/**
 * @brief Get backend-aware normalized key
 *
 * Returns the normalized identity key used for equality and advisory locking.
 * The string is valid until the handle is destroyed.
 *
 * @param handle File handle (required)
 * @param status Error reporting (required)
 * @return Borrowed pointer to normalized key or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns borrowed pointer - do NOT free
 */
ENTROPY_API const char* entropy_file_handle_normalized_key(
    entropy_FileHandle handle,
    EntropyStatus* status
);

#ifdef __cplusplus
} // extern "C"
#endif
