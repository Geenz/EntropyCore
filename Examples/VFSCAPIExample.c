/**
 * @file VFSCAPIExample.c
 * @brief Comprehensive example of VirtualFileSystem C API usage
 *
 * Demonstrates:
 * - Creating VFS with WorkContractGroup and WorkService
 * - File operations (read, write, delete)
 * - Directory operations (create, list, remove)
 * - Write batch for atomic multi-line editing
 * - Error handling with status codes
 * - Proper resource cleanup
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* EntropyCore C API headers */
#include "Logging/CLogger.h"
#include "entropy/entropy_directory_handle.h"
#include "entropy/entropy_file_handle.h"
#include "entropy/entropy_virtual_file_system.h"
#include "entropy/entropy_work_contract_group.h"
#include "entropy/entropy_work_service.h"
#include "entropy/entropy_write_batch.h"

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void check_status(EntropyStatus status, const char* operation) {
    if (status != ENTROPY_OK) {
        ENTROPY_LOG_ERROR_F("Operation '%s' failed with status %d", operation, status);
        exit(1);
    }
}

static void print_file_error(const EntropyFileErrorInfo* err) {
    printf("  Error: %s\n", entropy_file_error_to_string(err->code));
    printf("  Message: %s\n", err->message);
    if (err->path && strlen(err->path) > 0) {
        printf("  Path: %s\n", err->path);
    }
    if (err->system_error_code != 0) {
        printf("  System error: %d (%s)\n", err->system_error_code,
               err->system_error_category ? err->system_error_category : "unknown");
    }
}

/* ============================================================================
 * Example: Basic File Operations
 * ============================================================================ */

static void example_basic_file_ops(entropy_VirtualFileSystem vfs) {
    ENTROPY_LOG_INFO_CAT_F("Example", "=== Basic File Operations ===");
    EntropyStatus status = ENTROPY_OK;

    /* Create a file handle */
    const char* test_file = "/tmp/entropy_test_basic.txt";
    entropy_FileHandle fh = entropy_vfs_create_file_handle(vfs, test_file, &status);
    check_status(status, "create file handle");

    /* Write text to file */
    ENTROPY_LOG_INFO_CAT_F("Example", "Writing to file: %s", test_file);
    const char* text = "Hello from EntropyCore VFS C API!\nLine 2\nLine 3";

    EntropyWriteOptions write_opts;
    entropy_write_options_init(&write_opts);
    write_opts.truncate = ENTROPY_TRUE;
    write_opts.create_if_missing = ENTROPY_TRUE;

    entropy_FileOperationHandle write_op =
        entropy_file_handle_write_all_text_with_options(fh, text, &write_opts, &status);
    check_status(status, "write file");

    entropy_file_operation_handle_wait(write_op, &status);
    check_status(status, "wait for write");

    EntropyFileOpStatus write_status = entropy_file_operation_handle_status(write_op, &status);
    if (write_status != ENTROPY_FILE_OP_COMPLETE) {
        const EntropyFileErrorInfo* err = entropy_file_operation_handle_error_info(write_op, &status);
        ENTROPY_LOG_ERROR_F("Write failed:");
        print_file_error(err);
    } else {
        uint64_t bytes_written = entropy_file_operation_handle_bytes_written(write_op, &status);
        ENTROPY_LOG_INFO_CAT_F("Example", "Wrote %llu bytes", (unsigned long long)bytes_written);
    }

    entropy_file_operation_handle_destroy(write_op);

    /* Read file back */
    ENTROPY_LOG_INFO_CAT_F("Example", "Reading file back...");
    entropy_FileOperationHandle read_op = entropy_file_handle_read_all(fh, &status);
    check_status(status, "read file");

    entropy_file_operation_handle_wait(read_op, &status);
    check_status(status, "wait for read");

    EntropyFileOpStatus read_status = entropy_file_operation_handle_status(read_op, &status);
    if (read_status == ENTROPY_FILE_OP_COMPLETE) {
        const char* content = entropy_file_operation_handle_contents_text(read_op, &status);
        ENTROPY_LOG_INFO_CAT_F("Example", "Read content:\n%s", content);
    } else {
        const EntropyFileErrorInfo* err = entropy_file_operation_handle_error_info(read_op, &status);
        ENTROPY_LOG_ERROR_F("Read failed:");
        print_file_error(err);
    }

    entropy_file_operation_handle_destroy(read_op);

    /* Remove file */
    ENTROPY_LOG_INFO_CAT_F("Example", "Removing file...");
    entropy_FileOperationHandle remove_op = entropy_file_handle_remove(fh, &status);
    check_status(status, "remove file");

    entropy_file_operation_handle_wait(remove_op, &status);
    check_status(status, "wait for remove");

    if (entropy_file_operation_handle_status(remove_op, &status) == ENTROPY_FILE_OP_COMPLETE) {
        ENTROPY_LOG_INFO_CAT_F("Example", "File removed successfully");
    }

    entropy_file_operation_handle_destroy(remove_op);
    entropy_file_handle_destroy(fh);
}

/* ============================================================================
 * Example: Directory Operations
 * ============================================================================ */

static void example_directory_ops(entropy_VirtualFileSystem vfs) {
    ENTROPY_LOG_INFO_CAT_F("Example", "=== Directory Operations ===");
    EntropyStatus status = ENTROPY_OK;

    /* Create directory handle */
    const char* test_dir = "/tmp/entropy_test_dir";
    entropy_DirectoryHandle dh = entropy_vfs_create_directory_handle(vfs, test_dir, &status);
    check_status(status, "create directory handle");

    /* Create directory */
    ENTROPY_LOG_INFO_CAT_F("Example", "Creating directory: %s", test_dir);
    entropy_FileOperationHandle create_op = entropy_directory_handle_create(dh, ENTROPY_TRUE, &status);
    check_status(status, "create directory");

    entropy_file_operation_handle_wait(create_op, &status);
    check_status(status, "wait for create");

    if (entropy_file_operation_handle_status(create_op, &status) == ENTROPY_FILE_OP_COMPLETE) {
        ENTROPY_LOG_INFO_CAT_F("Example", "Directory created successfully");
    }
    entropy_file_operation_handle_destroy(create_op);

    /* Create some test files in the directory */
    const char* file_names[] = {"file1.txt", "file2.txt", "file3.txt"};
    for (int i = 0; i < 3; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", test_dir, file_names[i]);

        entropy_FileHandle fh = entropy_vfs_create_file_handle(vfs, path, &status);
        check_status(status, "create file handle for test file");

        char content[64];
        snprintf(content, sizeof(content), "Test file %d content", i + 1);

        EntropyWriteOptions opts;
        entropy_write_options_init(&opts);
        opts.create_if_missing = ENTROPY_TRUE;
        opts.truncate = ENTROPY_TRUE;

        entropy_FileOperationHandle write_op =
            entropy_file_handle_write_all_text_with_options(fh, content, &opts, &status);
        entropy_file_operation_handle_wait(write_op, &status);
        entropy_file_operation_handle_destroy(write_op);
        entropy_file_handle_destroy(fh);
    }

    /* List directory contents */
    ENTROPY_LOG_INFO_CAT_F("Example", "Listing directory contents...");
    EntropyListDirectoryOptions list_opts;
    entropy_list_directory_options_init(&list_opts);
    list_opts.recursive = ENTROPY_FALSE;
    list_opts.include_hidden = ENTROPY_FALSE;
    list_opts.sort_by = ENTROPY_SORT_BY_NAME;

    entropy_FileOperationHandle list_op = entropy_directory_handle_list(dh, &list_opts, &status);
    check_status(status, "list directory");

    entropy_file_operation_handle_wait(list_op, &status);
    check_status(status, "wait for list");

    if (entropy_file_operation_handle_status(list_op, &status) == ENTROPY_FILE_OP_COMPLETE) {
        size_t count;
        const EntropyDirectoryEntry* entries =
            entropy_file_operation_handle_directory_entries(list_op, &count, &status);

        ENTROPY_LOG_INFO_CAT_F("Example", "Found %zu entries:", count);
        for (size_t i = 0; i < count; i++) {
            printf("  [%zu] %s (%llu bytes)\n", i, entries[i].name, (unsigned long long)entries[i].metadata.size);
        }
    }

    entropy_file_operation_handle_destroy(list_op);

    /* Remove directory recursively */
    ENTROPY_LOG_INFO_CAT_F("Example", "Removing directory recursively...");
    entropy_FileOperationHandle remove_op = entropy_directory_handle_remove(dh, ENTROPY_TRUE, &status);
    check_status(status, "remove directory");

    entropy_file_operation_handle_wait(remove_op, &status);
    check_status(status, "wait for remove");

    if (entropy_file_operation_handle_status(remove_op, &status) == ENTROPY_FILE_OP_COMPLETE) {
        ENTROPY_LOG_INFO_CAT_F("Example", "Directory removed successfully");
    }

    entropy_file_operation_handle_destroy(remove_op);
    entropy_directory_handle_destroy(dh);
}

/* ============================================================================
 * Example: Write Batch
 * ============================================================================ */

static void example_write_batch(entropy_VirtualFileSystem vfs) {
    ENTROPY_LOG_INFO_CAT_F("Example", "=== Write Batch Operations ===");
    EntropyStatus status = ENTROPY_OK;

    const char* batch_file = "/tmp/entropy_test_batch.txt";

    /* Create initial file content */
    ENTROPY_LOG_INFO_CAT_F("Example", "Creating initial file for batch editing...");
    entropy_FileHandle fh = entropy_vfs_create_file_handle(vfs, batch_file, &status);
    check_status(status, "create file handle");

    const char* initial = "Line 0\nLine 1\nLine 2\nLine 3\nLine 4";
    EntropyWriteOptions opts;
    entropy_write_options_init(&opts);
    opts.create_if_missing = ENTROPY_TRUE;
    opts.truncate = ENTROPY_TRUE;

    entropy_FileOperationHandle write_op = entropy_file_handle_write_all_text_with_options(fh, initial, &opts, &status);
    entropy_file_operation_handle_wait(write_op, &status);
    entropy_file_operation_handle_destroy(write_op);

    /* Create write batch */
    ENTROPY_LOG_INFO_CAT_F("Example", "Creating write batch...");
    entropy_WriteBatch batch = entropy_vfs_create_write_batch(vfs, batch_file, &status);
    check_status(status, "create write batch");

    /* Queue batch operations */
    ENTROPY_LOG_INFO_CAT_F("Example", "Queueing batch operations...");
    entropy_write_batch_write_line(batch, 0, "MODIFIED Line 0", &status);
    check_status(status, "batch write line");

    entropy_write_batch_insert_line(batch, 2, "INSERTED Line", &status);
    check_status(status, "batch insert line");

    entropy_write_batch_append_line(batch, "APPENDED Line", &status);
    check_status(status, "batch append line");

    entropy_write_batch_delete_line(batch, 4, &status);
    check_status(status, "batch delete line");

    size_t pending = entropy_write_batch_pending_operations(batch, &status);
    ENTROPY_LOG_INFO_CAT_F("Example", "Pending operations: %zu", pending);

    /* Preview changes (optional) */
    ENTROPY_LOG_INFO_CAT_F("Example", "Previewing batch changes...");
    entropy_FileOperationHandle preview_op = entropy_write_batch_preview(batch, &status);
    check_status(status, "batch preview");

    entropy_file_operation_handle_wait(preview_op, &status);
    if (entropy_file_operation_handle_status(preview_op, &status) == ENTROPY_FILE_OP_COMPLETE) {
        const char* preview = entropy_file_operation_handle_contents_text(preview_op, &status);
        ENTROPY_LOG_INFO_CAT_F("Example", "Preview:\n%s", preview);
    }
    entropy_file_operation_handle_destroy(preview_op);

    /* Commit batch */
    ENTROPY_LOG_INFO_CAT_F("Example", "Committing batch...");
    entropy_FileOperationHandle commit_op = entropy_write_batch_commit(batch, &status);
    check_status(status, "batch commit");

    entropy_file_operation_handle_wait(commit_op, &status);
    if (entropy_file_operation_handle_status(commit_op, &status) == ENTROPY_FILE_OP_COMPLETE) {
        ENTROPY_LOG_INFO_CAT_F("Example", "Batch committed successfully");
    } else {
        const EntropyFileErrorInfo* err = entropy_file_operation_handle_error_info(commit_op, &status);
        ENTROPY_LOG_ERROR_F("Batch commit failed:");
        print_file_error(err);
    }
    entropy_file_operation_handle_destroy(commit_op);

    /* Read back to verify */
    ENTROPY_LOG_INFO_CAT_F("Example", "Reading file after batch commit...");
    entropy_FileOperationHandle read_op = entropy_file_handle_read_all(fh, &status);
    entropy_file_operation_handle_wait(read_op, &status);
    if (entropy_file_operation_handle_status(read_op, &status) == ENTROPY_FILE_OP_COMPLETE) {
        const char* final = entropy_file_operation_handle_contents_text(read_op, &status);
        ENTROPY_LOG_INFO_CAT_F("Example", "Final content:\n%s", final);
    }
    entropy_file_operation_handle_destroy(read_op);

    /* Cleanup */
    entropy_FileOperationHandle remove_op = entropy_file_handle_remove(fh, &status);
    entropy_file_operation_handle_wait(remove_op, &status);
    entropy_file_operation_handle_destroy(remove_op);

    entropy_write_batch_destroy(batch);
    entropy_file_handle_destroy(fh);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    ENTROPY_LOG_INFO_F("=== EntropyCore VFS C API Example ===");

    EntropyStatus status = ENTROPY_OK;

    /* Create work contract group */
    ENTROPY_LOG_INFO_F("Creating WorkContractGroup...");
    entropy_WorkContractGroup group = entropy_work_contract_group_create(2048, "VFS_Example", &status);
    check_status(status, "create work contract group");

    /* Create work service */
    ENTROPY_LOG_INFO_F("Creating WorkService...");
    EntropyWorkServiceConfig service_config;
    entropy_work_service_config_init(&service_config);
    service_config.thread_count = 4;

    entropy_WorkService service = entropy_work_service_create(&service_config, &status);
    check_status(status, "create work service");

    /* Register group with service and start */
    entropy_work_service_add_group(service, group, &status);
    check_status(status, "add group to service");

    entropy_work_service_start(service, &status);
    check_status(status, "start work service");

    /* Create VFS */
    ENTROPY_LOG_INFO_F("Creating VirtualFileSystem...");
    entropy_VirtualFileSystem vfs = entropy_vfs_create(group, &status);
    check_status(status, "create VFS");

    /* Run examples */
    example_basic_file_ops(vfs);
    example_directory_ops(vfs);
    example_write_batch(vfs);

    /* Cleanup */
    ENTROPY_LOG_INFO_F("Cleaning up...");
    entropy_vfs_destroy(vfs);

    entropy_work_service_stop(service, &status);
    check_status(status, "stop work service");

    entropy_work_service_destroy(service);
    entropy_work_contract_group_destroy(group);

    ENTROPY_LOG_INFO_F("=== Example completed successfully ===");
    return 0;
}
