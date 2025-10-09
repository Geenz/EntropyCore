#pragma once

/**
 * @file entropy_directory_handle.h
 * @brief C API for DirectoryHandle - value-semantic directory operations
 *
 * DirectoryHandle provides a copyable reference to a directory path routed
 * through a VirtualFileSystem backend. All operations are asynchronous; call
 * wait() on the returned FileOperationHandle to block until completion.
 */

#include "entropy/entropy_vfs_types.h"
#include "entropy/entropy_file_operation_handle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DirectoryHandle Lifecycle
 * ============================================================================ */

/**
 * @brief Clone a directory handle
 *
 * Creates a copy that references the same directory path and VFS. Both handles
 * remain valid and must be destroyed independently.
 *
 * @param handle Handle to clone (required)
 * @param status Error reporting (required)
 * @return Owned clone or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_directory_handle_destroy()
 */
ENTROPY_API entropy_DirectoryHandle entropy_directory_handle_clone(
    entropy_DirectoryHandle handle,
    EntropyStatus* status
);

/**
 * @brief Destroy a directory handle
 *
 * Releases the handle. Does not affect the underlying directory or any pending operations.
 *
 * @param handle Handle to destroy (can be NULL)
 * @threadsafety Thread-safe
 */
ENTROPY_API void entropy_directory_handle_destroy(entropy_DirectoryHandle handle);

/* ============================================================================
 * Directory Operations
 * ============================================================================ */

/**
 * @brief Create the directory at this path
 *
 * Asynchronously creates the directory. Backend-specific behavior:
 * - LocalFileSystem: Always creates all parent directories (mkdir -p semantics),
 *   regardless of createParents parameter. Operation is idempotent.
 * - Object storage: May create 0-byte marker object (future).
 *
 * @param handle Directory handle (required)
 * @param create_parents Hint to create parent directories (currently ignored by most backends)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 *
 * @code
 * entropy_FileOperationHandle op = entropy_directory_handle_create(dh, ENTROPY_TRUE, &status);
 * entropy_file_operation_handle_wait(op, &status);
 * if (entropy_file_operation_handle_status(op, &status) == ENTROPY_FILE_OP_COMPLETE) {
 *     printf("Directory created\n");
 * }
 * entropy_file_operation_handle_destroy(op);
 * @endcode
 */
ENTROPY_API entropy_FileOperationHandle entropy_directory_handle_create(
    entropy_DirectoryHandle handle,
    EntropyBool create_parents,
    EntropyStatus* status
);

/**
 * @brief Remove the directory at this path
 *
 * Asynchronously removes the directory. Backend-specific behavior:
 * - LocalFileSystem: Fails if non-empty unless recursive=true
 * - Object storage: Deletes all objects with this prefix (potentially expensive)
 *
 * @param handle Directory handle (required)
 * @param recursive If true, remove contents recursively
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_directory_handle_remove(
    entropy_DirectoryHandle handle,
    EntropyBool recursive,
    EntropyStatus* status
);

/**
 * @brief List the contents of this directory
 *
 * Asynchronously lists directory contents. Call entropy_file_operation_handle_directory_entries()
 * after wait() to access results.
 *
 * @param handle Directory handle (required)
 * @param options Listing options (required, copied)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 *
 * @code
 * EntropyListDirectoryOptions opts;
 * entropy_list_directory_options_init(&opts);
 * opts.recursive = ENTROPY_TRUE;
 *
 * entropy_FileOperationHandle op = entropy_directory_handle_list(dh, &opts, &status);
 * entropy_file_operation_handle_wait(op, &status);
 *
 * size_t count;
 * const EntropyDirectoryEntry* entries =
 *     entropy_file_operation_handle_directory_entries(op, &count, &status);
 * for (size_t i = 0; i < count; i++) {
 *     printf("%s\n", entries[i].name);
 * }
 * entropy_file_operation_handle_destroy(op);
 * @endcode
 */
ENTROPY_API entropy_FileOperationHandle entropy_directory_handle_list(
    entropy_DirectoryHandle handle,
    const EntropyListDirectoryOptions* options,
    EntropyStatus* status
);

/**
 * @brief Retrieve metadata for this directory
 *
 * Asynchronously retrieves directory metadata. Call entropy_file_operation_handle_metadata()
 * after wait() to access results.
 *
 * @param handle Directory handle (required)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_directory_handle_get_metadata(
    entropy_DirectoryHandle handle,
    EntropyStatus* status
);

/* ============================================================================
 * Metadata Access
 * ============================================================================ */

/**
 * @brief Get backend-aware normalized key
 *
 * Returns the normalized identity key used for equality. The string is valid
 * until the handle is destroyed.
 *
 * @param handle Directory handle (required)
 * @param status Error reporting (required)
 * @return Borrowed pointer to normalized key or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns borrowed pointer - do NOT free
 */
ENTROPY_API const char* entropy_directory_handle_normalized_key(
    entropy_DirectoryHandle handle,
    EntropyStatus* status
);

#ifdef __cplusplus
} // extern "C"
#endif
