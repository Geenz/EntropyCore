/**
 * @file entropy_vfs_types_c.cpp
 * @brief Implementation of VFS type helpers and conversions
 */

#include <limits.h>
#include <string.h>

#include "entropy/entropy_vfs_types.h"

extern "C" {

/* ============================================================================
 * Config Initialization
 * ============================================================================ */

void entropy_vfs_config_init(EntropyVFSConfig* config) {
    if (!config) return;

    config->serialize_writes_per_path = ENTROPY_TRUE;
    config->max_write_locks_cached = 1024;
    config->write_lock_timeout_minutes = 5;
    config->default_create_parent_dirs = ENTROPY_FALSE;
    config->advisory_acquire_timeout_ms = 5000;
    config->default_use_lock_file = ENTROPY_FALSE;
    config->lock_acquire_timeout_ms = 5000;
    config->lock_suffix = ".lock";
}

void entropy_read_options_init(EntropyReadOptions* options) {
    if (!options) return;

    options->offset = 0;
    options->length = 0;  // 0 = read to EOF
    options->binary = ENTROPY_TRUE;
}

void entropy_write_options_init(EntropyWriteOptions* options) {
    if (!options) return;

    options->offset = 0;
    options->append = ENTROPY_FALSE;
    options->create_if_missing = ENTROPY_TRUE;
    options->truncate = ENTROPY_FALSE;
    options->create_parent_dirs = -1;    // Use VFS default
    options->ensure_final_newline = -1;  // Preserve original
    options->fsync = ENTROPY_FALSE;
    options->use_lock_file = -1;   // Use VFS default
    options->lock_timeout_ms = 0;  // Use VFS default
    options->lock_suffix = NULL;   // Use VFS default
}

void entropy_stream_options_init(EntropyStreamOptions* options) {
    if (!options) return;

    options->mode = ENTROPY_STREAM_MODE_READ;
    options->buffered = ENTROPY_TRUE;
    options->buffer_size = 65536;  // 64KB
}

void entropy_list_directory_options_init(EntropyListDirectoryOptions* options) {
    if (!options) return;

    options->recursive = ENTROPY_FALSE;
    options->follow_symlinks = ENTROPY_TRUE;
    options->max_depth = SIZE_MAX;
    options->glob_pattern = NULL;
    options->include_hidden = ENTROPY_FALSE;
    options->sort_by = ENTROPY_SORT_NONE;
    options->max_results = 0;  // Unlimited
}

/* ============================================================================
 * String Conversions
 * ============================================================================ */

const char* entropy_file_op_status_to_string(EntropyFileOpStatus status) {
    switch (status) {
        case ENTROPY_FILE_OP_PENDING:
            return "Pending";
        case ENTROPY_FILE_OP_RUNNING:
            return "Running";
        case ENTROPY_FILE_OP_PARTIAL:
            return "Partial";
        case ENTROPY_FILE_OP_COMPLETE:
            return "Complete";
        case ENTROPY_FILE_OP_FAILED:
            return "Failed";
        default:
            return "Unknown";
    }
}

const char* entropy_file_error_to_string(EntropyFileError error) {
    switch (error) {
        case ENTROPY_FILE_ERROR_NONE:
            return "None";
        case ENTROPY_FILE_ERROR_FILE_NOT_FOUND:
            return "FileNotFound";
        case ENTROPY_FILE_ERROR_ACCESS_DENIED:
            return "AccessDenied";
        case ENTROPY_FILE_ERROR_DISK_FULL:
            return "DiskFull";
        case ENTROPY_FILE_ERROR_INVALID_PATH:
            return "InvalidPath";
        case ENTROPY_FILE_ERROR_IO_ERROR:
            return "IOError";
        case ENTROPY_FILE_ERROR_NETWORK_ERROR:
            return "NetworkError";
        case ENTROPY_FILE_ERROR_TIMEOUT:
            return "Timeout";
        case ENTROPY_FILE_ERROR_CONFLICT:
            return "Conflict";
        case ENTROPY_FILE_ERROR_UNKNOWN:
            return "Unknown";
        default:
            return "Unknown";
    }
}

}  // extern "C"
