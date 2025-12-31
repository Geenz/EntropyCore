#pragma once

/**
 * @file entropy_write_batch.h
 * @brief C API for WriteBatch - atomic multi-line file editing
 *
 * WriteBatch collects multiple write operations and applies them atomically
 * in a single file operation. This allows efficient batch processing of file
 * modifications without repeatedly reading and writing the entire file.
 */

#include "entropy/entropy_file_operation_handle.h"
#include "entropy/entropy_vfs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * WriteBatch Lifecycle
 * ============================================================================ */

/**
 * @brief Destroy a write batch
 *
 * Releases the batch. Any uncommitted operations are discarded.
 *
 * @param batch Batch to destroy (can be NULL)
 * @threadsafety NOT thread-safe - do not use batch concurrently
 */
ENTROPY_API void entropy_write_batch_destroy(entropy_WriteBatch batch);

/* ============================================================================
 * Line Operations
 * ============================================================================ */

/**
 * @brief Overwrite a line at index (0-based)
 *
 * Queues an operation to replace the line at the specified index. If the index
 * is beyond EOF, the file is extended with blank lines.
 *
 * @param batch Write batch (required)
 * @param line_number Line index to write (0-based)
 * @param content New line content (required, without newline)
 * @param status Error reporting (required)
 * @threadsafety NOT thread-safe - do not use batch concurrently
 *
 * @code
 * entropy_WriteBatch batch = entropy_vfs_create_write_batch(vfs, "file.txt", &status);
 * entropy_write_batch_write_line(batch, 0, "First line", &status);
 * entropy_write_batch_write_line(batch, 1, "Second line", &status);
 * entropy_FileOperationHandle op = entropy_write_batch_commit(batch, &status);
 * entropy_file_operation_handle_wait(op, &status);
 * entropy_file_operation_handle_destroy(op);
 * entropy_write_batch_destroy(batch);
 * @endcode
 */
ENTROPY_API void entropy_write_batch_write_line(entropy_WriteBatch batch, size_t line_number, const char* content,
                                                EntropyStatus* status);

/**
 * @brief Insert a line at index (shifts existing lines down)
 *
 * Queues an operation to insert a new line, pushing existing lines down.
 *
 * @param batch Write batch (required)
 * @param line_number Insert position (0-based)
 * @param content Line content (required, without newline)
 * @param status Error reporting (required)
 * @threadsafety NOT thread-safe - do not use batch concurrently
 */
ENTROPY_API void entropy_write_batch_insert_line(entropy_WriteBatch batch, size_t line_number, const char* content,
                                                 EntropyStatus* status);

/**
 * @brief Delete a line at index (shifts remaining lines up)
 *
 * Queues an operation to remove a line, pulling subsequent lines up.
 *
 * @param batch Write batch (required)
 * @param line_number Line to delete (0-based)
 * @param status Error reporting (required)
 * @threadsafety NOT thread-safe - do not use batch concurrently
 */
ENTROPY_API void entropy_write_batch_delete_line(entropy_WriteBatch batch, size_t line_number, EntropyStatus* status);

/**
 * @brief Append a line to the end of the file
 *
 * Queues an operation to add a new line at the end.
 *
 * @param batch Write batch (required)
 * @param content Line content (required, without newline)
 * @param status Error reporting (required)
 * @threadsafety NOT thread-safe - do not use batch concurrently
 */
ENTROPY_API void entropy_write_batch_append_line(entropy_WriteBatch batch, const char* content, EntropyStatus* status);

/**
 * @brief Replace entire file content with text
 *
 * Queues an operation to replace the entire file. May contain multiple lines.
 *
 * @param batch Write batch (required)
 * @param content New file content (required)
 * @param status Error reporting (required)
 * @threadsafety NOT thread-safe - do not use batch concurrently
 */
ENTROPY_API void entropy_write_batch_replace_all(entropy_WriteBatch batch, const char* content, EntropyStatus* status);

/**
 * @brief Clear the file (truncate to zero length)
 *
 * Queues an operation to empty the file.
 *
 * @param batch Write batch (required)
 * @param status Error reporting (required)
 * @threadsafety NOT thread-safe - do not use batch concurrently
 */
ENTROPY_API void entropy_write_batch_clear(entropy_WriteBatch batch, EntropyStatus* status);

/* ============================================================================
 * Execution
 * ============================================================================ */

/**
 * @brief Apply all pending operations atomically
 *
 * Executes all queued operations in a single atomic write. Uses VFS defaults
 * for parent directory creation and preserves the original line-ending style.
 *
 * @param batch Write batch (required)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety NOT thread-safe - do not use batch concurrently
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 *
 * @code
 * entropy_WriteBatch batch = entropy_vfs_create_write_batch(vfs, "file.txt", &status);
 * entropy_write_batch_write_line(batch, 0, "Hello", &status);
 * entropy_write_batch_append_line(batch, "World", &status);
 * entropy_FileOperationHandle op = entropy_write_batch_commit(batch, &status);
 * entropy_file_operation_handle_wait(op, &status);
 * if (entropy_file_operation_handle_status(op, &status) == ENTROPY_FILE_OP_COMPLETE) {
 *     printf("Batch committed\n");
 * }
 * entropy_file_operation_handle_destroy(op);
 * entropy_write_batch_destroy(batch);
 * @endcode
 */
ENTROPY_API entropy_FileOperationHandle entropy_write_batch_commit(entropy_WriteBatch batch, EntropyStatus* status);

/**
 * @brief Apply all pending operations atomically with options
 *
 * @param batch Write batch (required)
 * @param options Write options (required, copied)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety NOT thread-safe - do not use batch concurrently
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_write_batch_commit_with_options(entropy_WriteBatch batch,
                                                                                const EntropyWriteOptions* options,
                                                                                EntropyStatus* status);

/**
 * @brief Build the resulting content without writing it
 *
 * Returns what the file would look like after applying operations. Useful for
 * debugging. Call entropy_file_operation_handle_contents_text() after wait().
 *
 * @param batch Write batch (required)
 * @param status Error reporting (required)
 * @return Owned operation handle or NULL on error
 * @threadsafety NOT thread-safe - do not use batch concurrently
 * @ownership Returns owned pointer - must call entropy_file_operation_handle_destroy()
 */
ENTROPY_API entropy_FileOperationHandle entropy_write_batch_preview(entropy_WriteBatch batch, EntropyStatus* status);

/* ============================================================================
 * Query and Management
 * ============================================================================ */

/**
 * @brief Get the number of pending operations
 *
 * @param batch Write batch (required)
 * @param status Error reporting (required)
 * @return Number of operations queued, or 0 on error
 * @threadsafety Thread-safe
 */
ENTROPY_API size_t entropy_write_batch_pending_operations(entropy_WriteBatch batch, EntropyStatus* status);

/**
 * @brief Check if the batch is empty
 *
 * @param batch Write batch (required)
 * @param status Error reporting (required)
 * @return True if no operations pending, false otherwise
 * @threadsafety Thread-safe
 */
ENTROPY_API EntropyBool entropy_write_batch_is_empty(entropy_WriteBatch batch, EntropyStatus* status);

/**
 * @brief Clear all pending operations without writing
 *
 * Removes all queued operations. The batch can be reused.
 *
 * @param batch Write batch (required)
 * @param status Error reporting (required)
 * @threadsafety NOT thread-safe - do not use batch concurrently
 */
ENTROPY_API void entropy_write_batch_reset(entropy_WriteBatch batch, EntropyStatus* status);

/**
 * @brief Get the target file path for this batch
 *
 * Returns a borrowed pointer valid until the batch is destroyed.
 *
 * @param batch Write batch (required)
 * @param status Error reporting (required)
 * @return Borrowed pointer to path or NULL on error
 * @threadsafety Thread-safe
 * @ownership Returns borrowed pointer - do NOT free
 */
ENTROPY_API const char* entropy_write_batch_get_path(entropy_WriteBatch batch, EntropyStatus* status);

#ifdef __cplusplus
}  // extern "C"
#endif
