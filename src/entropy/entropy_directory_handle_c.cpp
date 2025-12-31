/**
 * @file entropy_directory_handle_c.cpp
 * @brief Implementation of DirectoryHandle C API
 */

#include <new>
#include <string>

#include "VirtualFileSystem/DirectoryHandle.h"
#include "VirtualFileSystem/FileOperationHandle.h"
#include "VirtualFileSystem/IFileSystemBackend.h"
#include "entropy/entropy_directory_handle.h"

using namespace EntropyEngine::Core::IO;

/* ============================================================================
 * Forward Declaration
 * ============================================================================ */

// Helper from entropy_file_operation_handle_c.cpp
extern "C" entropy_FileOperationHandle wrap_file_operation_handle(FileOperationHandle&& handle);

/* ============================================================================
 * Exception Translation
 * ============================================================================ */

static void translate_exception(EntropyStatus* status) {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        if (status) *status = ENTROPY_ERR_NO_MEMORY;
    } catch (const std::invalid_argument&) {
        if (status) *status = ENTROPY_ERR_INVALID_ARG;
    } catch (...) {
        if (status) *status = ENTROPY_ERR_UNKNOWN;
    }
}

/* ============================================================================
 * Type Conversions
 * ============================================================================ */

static ListDirectoryOptions to_cpp_list_options(const EntropyListDirectoryOptions* opts) {
    ListDirectoryOptions lo;
    lo.recursive = opts->recursive != ENTROPY_FALSE;
    lo.followSymlinks = opts->follow_symlinks != ENTROPY_FALSE;
    lo.maxDepth = opts->max_depth;
    lo.includeHidden = opts->include_hidden != ENTROPY_FALSE;
    lo.maxResults = opts->max_results;

    if (opts->glob_pattern) {
        lo.globPattern = opts->glob_pattern;
    }

    switch (opts->sort_by) {
        case ENTROPY_SORT_BY_NAME:
            lo.sortBy = ListDirectoryOptions::ByName;
            break;
        case ENTROPY_SORT_BY_SIZE:
            lo.sortBy = ListDirectoryOptions::BySize;
            break;
        case ENTROPY_SORT_BY_MODIFIED_TIME:
            lo.sortBy = ListDirectoryOptions::ByModifiedTime;
            break;
        default:
            lo.sortBy = ListDirectoryOptions::None;
            break;
    }

    return lo;
}

/* ============================================================================
 * DirectoryHandle Implementation
 * ============================================================================ */

extern "C" {

entropy_DirectoryHandle entropy_directory_handle_clone(entropy_DirectoryHandle handle, EntropyStatus* status) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<DirectoryHandle*>(handle);
        auto* clone = new (std::nothrow) DirectoryHandle(*cpp_handle);
        if (!clone) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        *status = ENTROPY_OK;
        return reinterpret_cast<entropy_DirectoryHandle>(clone);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

void entropy_directory_handle_destroy(entropy_DirectoryHandle handle) {
    if (!handle) return;
    auto* cpp_handle = reinterpret_cast<DirectoryHandle*>(handle);
    delete cpp_handle;
}

entropy_FileOperationHandle entropy_directory_handle_create(entropy_DirectoryHandle handle, EntropyBool create_parents,
                                                            EntropyStatus* status) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<DirectoryHandle*>(handle);
        FileOperationHandle op = cpp_handle->create(create_parents != ENTROPY_FALSE);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_directory_handle_remove(entropy_DirectoryHandle handle, EntropyBool recursive,
                                                            EntropyStatus* status) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<DirectoryHandle*>(handle);
        FileOperationHandle op = cpp_handle->remove(recursive != ENTROPY_FALSE);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_directory_handle_list(entropy_DirectoryHandle handle,
                                                          const EntropyListDirectoryOptions* options,
                                                          EntropyStatus* status) {
    if (!status) return nullptr;
    if (!handle || !options) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<DirectoryHandle*>(handle);
        ListDirectoryOptions opts = to_cpp_list_options(options);
        FileOperationHandle op = cpp_handle->list(opts);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_directory_handle_get_metadata(entropy_DirectoryHandle handle,
                                                                  EntropyStatus* status) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<DirectoryHandle*>(handle);
        FileOperationHandle op = cpp_handle->getMetadata();
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

const char* entropy_directory_handle_normalized_key(entropy_DirectoryHandle handle, EntropyStatus* status) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<DirectoryHandle*>(handle);
        *status = ENTROPY_OK;
        return cpp_handle->normalizedKey().c_str();
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

}  // extern "C"
