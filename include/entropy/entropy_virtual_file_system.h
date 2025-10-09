#pragma once

/**
 * @file entropy_virtual_file_system.h
 * @brief C API for VirtualFileSystem - main facade for file operations
 *
 * VirtualFileSystem routes file operations to pluggable backends and provides
 * ergonomic helpers for asynchronous file I/O. All operations execute via a
 * WorkContractGroup for parallelism.
 */

#include "entropy/entropy_vfs_types.h"
#include "entropy/entropy_work_contract_group.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * VirtualFileSystem Lifecycle
 * ============================================================================ */

/**
 * @brief Create a VirtualFileSystem with default config
 *
 * Creates a VFS instance bound to a WorkContractGroup for async execution.
 * The VFS does not own the group; caller must ensure group outlives VFS.
 *
 * @param group Work contract group for async operations (required, borrowed)
 * @param status Error reporting (required)
 * @return Owned VFS handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_vfs_destroy()
 *
 * @code
 * entropy_WorkContractGroup group = entropy_work_contract_group_create(1024, "VFS", &status);
 * entropy_VirtualFileSystem vfs = entropy_vfs_create(group, &status);
 * // ... use vfs ...
 * entropy_vfs_destroy(vfs);
 * entropy_work_contract_group_destroy(group);
 * @endcode
 */
ENTROPY_API entropy_VirtualFileSystem entropy_vfs_create(
    entropy_WorkContractGroup group,
    EntropyStatus* status
);

/**
 * @brief Create a VirtualFileSystem with custom configuration
 *
 * @param group Work contract group for async operations (required, borrowed)
 * @param config Configuration options (required, copied)
 * @param status Error reporting (required)
 * @return Owned VFS handle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_vfs_destroy()
 */
ENTROPY_API entropy_VirtualFileSystem entropy_vfs_create_with_config(
    entropy_WorkContractGroup group,
    const EntropyVFSConfig* config,
    EntropyStatus* status
);

/**
 * @brief Destroy a VirtualFileSystem and release resources
 *
 * Destroys the VFS. Any outstanding file/directory handles or operation handles
 * remain valid but will fail if used after VFS destruction.
 *
 * @param vfs VFS to destroy (can be NULL)
 * @threadsafety NOT thread-safe - caller must ensure no concurrent operations
 */
ENTROPY_API void entropy_vfs_destroy(entropy_VirtualFileSystem vfs);

/* ============================================================================
 * Handle Creation
 * ============================================================================ */

/**
 * @brief Create a file handle for the given path
 *
 * Creates a value-semantic handle bound to this VFS. The handle is copyable
 * (via entropy_file_handle_clone) and caches backend-normalized identity.
 *
 * @param vfs VFS instance (required)
 * @param path Target file path (required, copied)
 * @param status Error reporting (required)
 * @return Owned FileHandle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_file_handle_destroy()
 *
 * @code
 * entropy_FileHandle fh = entropy_vfs_create_file_handle(vfs, "test.txt", &status);
 * entropy_file_handle_destroy(fh);
 * @endcode
 */
ENTROPY_API entropy_FileHandle entropy_vfs_create_file_handle(
    entropy_VirtualFileSystem vfs,
    const char* path,
    EntropyStatus* status
);

/**
 * @brief Create a directory handle for the given path
 *
 * Creates a value-semantic handle bound to this VFS for directory operations.
 *
 * @param vfs VFS instance (required)
 * @param path Target directory path (required, copied)
 * @param status Error reporting (required)
 * @return Owned DirectoryHandle or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_directory_handle_destroy()
 */
ENTROPY_API entropy_DirectoryHandle entropy_vfs_create_directory_handle(
    entropy_VirtualFileSystem vfs,
    const char* path,
    EntropyStatus* status
);

/**
 * @brief Create a write batch for atomic multi-line file editing
 *
 * Write batches collect multiple line operations and apply them atomically
 * in a single serialized write.
 *
 * @param vfs VFS instance (required)
 * @param path Target file path (required, copied)
 * @param status Error reporting (required)
 * @return Owned WriteBatch or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns owned pointer - must call entropy_write_batch_destroy()
 *
 * @code
 * entropy_WriteBatch batch = entropy_vfs_create_write_batch(vfs, "file.txt", &status);
 * entropy_write_batch_write_line(batch, 0, "First line", &status);
 * entropy_FileOperationHandle op = entropy_write_batch_commit(batch, &status);
 * entropy_file_operation_handle_wait(op);
 * entropy_file_operation_handle_destroy(op);
 * entropy_write_batch_destroy(batch);
 * @endcode
 */
ENTROPY_API entropy_WriteBatch entropy_vfs_create_write_batch(
    entropy_VirtualFileSystem vfs,
    const char* path,
    EntropyStatus* status
);

#ifdef __cplusplus
} // extern "C"
#endif
