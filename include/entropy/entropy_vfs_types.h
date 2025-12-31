#pragma once

/**
 * @file entropy_vfs_types.h
 * @brief Type definitions for VirtualFileSystem C API
 *
 * This header defines all enums, structs, and opaque types for the VFS C API.
 * It follows the hourglass pattern: stable C89 ABI with internal C++ implementation.
 */

#include <stddef.h>
#include <stdint.h>

#include "Core/entropy_c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Error Codes - VFS-specific extensions to EntropyStatus
 * ============================================================================ */

#define ENTROPY_ERR_VFS_FILE_NOT_FOUND 100
#define ENTROPY_ERR_VFS_ACCESS_DENIED 101
#define ENTROPY_ERR_VFS_DISK_FULL 102
#define ENTROPY_ERR_VFS_INVALID_PATH 103
#define ENTROPY_ERR_VFS_IO_ERROR 104
#define ENTROPY_ERR_VFS_NETWORK_ERROR 105
#define ENTROPY_ERR_VFS_TIMEOUT 106
#define ENTROPY_ERR_VFS_CONFLICT 107

/* ============================================================================
 * Enumerations
 * ============================================================================ */

/**
 * @brief Status of a file operation
 */
typedef enum EntropyFileOpStatus
{
    ENTROPY_FILE_OP_PENDING = 0,  /**< Operation scheduled but not started */
    ENTROPY_FILE_OP_RUNNING = 1,  /**< Operation in progress */
    ENTROPY_FILE_OP_PARTIAL = 2,  /**< Operation partially completed (e.g., EOF on read) */
    ENTROPY_FILE_OP_COMPLETE = 3, /**< Operation completed successfully */
    ENTROPY_FILE_OP_FAILED = 4    /**< Operation failed (check error info) */
} EntropyFileOpStatus;

/**
 * @brief File operation error taxonomy
 */
typedef enum EntropyFileError
{
    ENTROPY_FILE_ERROR_NONE = 0,       /**< No error */
    ENTROPY_FILE_ERROR_FILE_NOT_FOUND, /**< Path does not exist when required */
    ENTROPY_FILE_ERROR_ACCESS_DENIED,  /**< Permission denied */
    ENTROPY_FILE_ERROR_DISK_FULL,      /**< No space left on device */
    ENTROPY_FILE_ERROR_INVALID_PATH,   /**< Malformed path or parent missing */
    ENTROPY_FILE_ERROR_IO_ERROR,       /**< Other I/O failure */
    ENTROPY_FILE_ERROR_NETWORK_ERROR,  /**< Remote backend transport failure */
    ENTROPY_FILE_ERROR_TIMEOUT,        /**< Operation timed out */
    ENTROPY_FILE_ERROR_CONFLICT,       /**< Contention detected */
    ENTROPY_FILE_ERROR_UNKNOWN         /**< Unknown error */
} EntropyFileError;

/**
 * @brief Stream access mode
 */
typedef enum EntropyStreamMode
{
    ENTROPY_STREAM_MODE_READ = 0,      /**< Read-only */
    ENTROPY_STREAM_MODE_WRITE = 1,     /**< Write-only */
    ENTROPY_STREAM_MODE_READ_WRITE = 2 /**< Read-write */
} EntropyStreamMode;

/**
 * @brief Directory listing sort order
 */
typedef enum EntropySortOrder
{
    ENTROPY_SORT_NONE = 0,            /**< No sorting */
    ENTROPY_SORT_BY_NAME = 1,         /**< Sort by name */
    ENTROPY_SORT_BY_SIZE = 2,         /**< Sort by file size */
    ENTROPY_SORT_BY_MODIFIED_TIME = 3 /**< Sort by modification time */
} EntropySortOrder;

/**
 * @brief Stream seek direction
 */
typedef enum EntropySeekDir
{
    ENTROPY_SEEK_BEGIN = 0,   /**< Seek from beginning */
    ENTROPY_SEEK_CURRENT = 1, /**< Seek from current position */
    ENTROPY_SEEK_END = 2      /**< Seek from end */
} EntropySeekDir;

/* ============================================================================
 * Opaque Types
 * ============================================================================ */

/** @brief Opaque handle to VirtualFileSystem */
typedef struct entropy_VirtualFileSystem_t* entropy_VirtualFileSystem;

/** @brief Opaque handle to FileHandle */
typedef struct entropy_FileHandle_t* entropy_FileHandle;

/** @brief Opaque handle to DirectoryHandle */
typedef struct entropy_DirectoryHandle_t* entropy_DirectoryHandle;

/** @brief Opaque handle to FileOperationHandle */
typedef struct entropy_FileOperationHandle_t* entropy_FileOperationHandle;

/** @brief Opaque handle to FileStream */
typedef struct entropy_FileStream_t* entropy_FileStream;

/** @brief Opaque handle to WriteBatch */
typedef struct entropy_WriteBatch_t* entropy_WriteBatch;

/* ============================================================================
 * Configuration Structures
 * ============================================================================ */

/**
 * @brief Configuration for VirtualFileSystem
 */
typedef struct EntropyVFSConfig
{
    /** Serialize writes per path (advisory locking) */
    EntropyBool serialize_writes_per_path;

    /** Maximum number of write locks to cache */
    size_t max_write_locks_cached;

    /** Timeout for unused write locks (minutes) */
    uint32_t write_lock_timeout_minutes;

    /** Default behavior for creating parent directories */
    EntropyBool default_create_parent_dirs;

    /** Advisory lock acquire timeout (milliseconds) */
    uint32_t advisory_acquire_timeout_ms;

    /** Use cross-process lock file by default */
    EntropyBool default_use_lock_file;

    /** Lock file acquire timeout (milliseconds) */
    uint32_t lock_acquire_timeout_ms;

    /** Lock file suffix (e.g., ".lock") - borrowed pointer, must remain valid */
    const char* lock_suffix;
} EntropyVFSConfig;

/**
 * @brief Options for file read operations
 */
typedef struct EntropyReadOptions
{
    /** Starting byte offset */
    uint64_t offset;

    /** Maximum bytes to read (0 = read to EOF) */
    size_t length;

    /** Open in binary mode */
    EntropyBool binary;
} EntropyReadOptions;

/**
 * @brief Options for file write operations
 */
typedef struct EntropyWriteOptions
{
    /** Starting byte offset (ignored if append=true) */
    uint64_t offset;

    /** Append to end of file */
    EntropyBool append;

    /** Create the file if it does not exist */
    EntropyBool create_if_missing;

    /** Truncate file before writing */
    EntropyBool truncate;

    /** Create parent directories (-1 = use VFS default, 0 = no, 1 = yes) */
    int32_t create_parent_dirs;

    /** Ensure final newline (-1 = preserve, 0 = no, 1 = yes) */
    int32_t ensure_final_newline;

    /** Force data to disk (fsync) */
    EntropyBool fsync;

    /** Use lock file (-1 = use VFS default, 0 = no, 1 = yes) */
    int32_t use_lock_file;

    /** Lock timeout override (milliseconds, 0 = use VFS default) */
    uint32_t lock_timeout_ms;

    /** Lock suffix override (NULL = use VFS default) */
    const char* lock_suffix;
} EntropyWriteOptions;

/**
 * @brief Options for stream operations
 */
typedef struct EntropyStreamOptions
{
    /** Access mode */
    EntropyStreamMode mode;

    /** Enable buffering */
    EntropyBool buffered;

    /** Buffer size in bytes */
    size_t buffer_size;
} EntropyStreamOptions;

/**
 * @brief Options for directory listing
 */
typedef struct EntropyListDirectoryOptions
{
    /** Recurse into subdirectories */
    EntropyBool recursive;

    /** Follow symlinks during traversal */
    EntropyBool follow_symlinks;

    /** Maximum recursion depth (SIZE_MAX = unlimited) */
    size_t max_depth;

    /** Glob pattern (e.g., "*.txt", NULL = no filter) */
    const char* glob_pattern;

    /** Include hidden files/directories */
    EntropyBool include_hidden;

    /** Sort order */
    EntropySortOrder sort_by;

    /** Maximum results for pagination (0 = unlimited) */
    size_t max_results;
} EntropyListDirectoryOptions;

/* ============================================================================
 * Result Structures
 * ============================================================================ */

/**
 * @brief Error information for failed operations
 */
typedef struct EntropyFileErrorInfo
{
    /** Error code */
    EntropyFileError code;

    /** Error message (borrowed pointer, valid until operation handle destroyed) */
    const char* message;

    /** Path that caused the error (borrowed pointer) */
    const char* path;

    /** System error code (if applicable, 0 = none) */
    int32_t system_error_code;

    /** System error category (borrowed pointer, NULL = none) */
    const char* system_error_category;
} EntropyFileErrorInfo;

/**
 * @brief File metadata
 */
typedef struct EntropyFileMetadata
{
    /** Full path */
    const char* path;

    /** File exists */
    EntropyBool exists;

    /** Is directory */
    EntropyBool is_directory;

    /** Is regular file */
    EntropyBool is_regular_file;

    /** Is symlink */
    EntropyBool is_symlink;

    /** File size in bytes */
    uint64_t size;

    /** Readable by someone */
    EntropyBool readable;

    /** Writable by someone */
    EntropyBool writable;

    /** Executable by someone */
    EntropyBool executable;

    /** Last modified time (Unix timestamp, milliseconds, -1 = unavailable) */
    int64_t last_modified_ms;

    /** MIME type (borrowed pointer, NULL = unavailable) */
    const char* mime_type;
} EntropyFileMetadata;

/**
 * @brief Directory entry with metadata
 */
typedef struct EntropyDirectoryEntry
{
    /** Filename only (borrowed pointer) */
    const char* name;

    /** Complete absolute path (borrowed pointer) */
    const char* full_path;

    /** Entry metadata */
    EntropyFileMetadata metadata;

    /** Is symlink */
    EntropyBool is_symlink;

    /** Symlink target (borrowed pointer, NULL = not a symlink) */
    const char* symlink_target;
} EntropyDirectoryEntry;

/**
 * @brief I/O operation result
 */
typedef struct EntropyIoResult
{
    /** Bytes transferred */
    size_t bytes_transferred;

    /** Operation completed successfully */
    EntropyBool complete;

    /** Error occurred */
    EntropyBool has_error;

    /** Error code (if has_error) */
    EntropyFileError error;
} EntropyIoResult;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * @brief Initialize VFS config with default values
 * @param config Config to initialize (required)
 */
ENTROPY_API void entropy_vfs_config_init(EntropyVFSConfig* config);

/**
 * @brief Initialize read options with default values
 * @param options Options to initialize (required)
 */
ENTROPY_API void entropy_read_options_init(EntropyReadOptions* options);

/**
 * @brief Initialize write options with default values
 * @param options Options to initialize (required)
 */
ENTROPY_API void entropy_write_options_init(EntropyWriteOptions* options);

/**
 * @brief Initialize stream options with default values
 * @param options Options to initialize (required)
 */
ENTROPY_API void entropy_stream_options_init(EntropyStreamOptions* options);

/**
 * @brief Initialize list directory options with default values
 * @param options Options to initialize (required)
 */
ENTROPY_API void entropy_list_directory_options_init(EntropyListDirectoryOptions* options);

/**
 * @brief Convert FileOpStatus to string
 * @param status Status to convert
 * @return Static string (do not free)
 * @threadsafety Thread-safe
 */
ENTROPY_API const char* entropy_file_op_status_to_string(EntropyFileOpStatus status);

/**
 * @brief Convert FileError to string
 * @param error Error to convert
 * @return Static string (do not free)
 * @threadsafety Thread-safe
 */
ENTROPY_API const char* entropy_file_error_to_string(EntropyFileError error);

#ifdef __cplusplus
}  // extern "C"
#endif
