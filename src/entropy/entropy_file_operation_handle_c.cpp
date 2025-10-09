/**
 * @file entropy_file_operation_handle_c.cpp
 * @brief Implementation of FileOperationHandle C API
 */

#include "entropy/entropy_file_operation_handle.h"
#include "VirtualFileSystem/FileOperationHandle.h"
#include <new>
#include <vector>
#include <string>
#include <mutex>
#include <system_error>

using namespace EntropyEngine::Core::IO;

/* ============================================================================
 * C-Compatible Result Cache
 * ============================================================================ */

// Cache structure to hold C-compatible versions of results
struct FileOpResultCache {
    std::mutex mutex; // Protect concurrent access to cache

    // Cached C structures
    EntropyFileMetadata c_metadata{};
    std::vector<EntropyDirectoryEntry> c_entries;
    EntropyFileErrorInfo c_error{};

    // Storage for string data (to keep pointers valid)
    std::string metadata_path;
    std::string metadata_mime;
    std::string error_message;
    std::string error_path;
    std::string error_category;
    std::vector<std::string> entry_names;
    std::vector<std::string> entry_paths;
    std::vector<std::string> entry_metadata_paths;
    std::vector<std::string> entry_symlink_targets;
    std::vector<std::string> entry_mime_types;

    bool metadata_cached = false;
    bool entries_cached = false;
    bool error_cached = false;
};

// Wrapper to hold C++ FileOperationHandle + C result cache
struct FileOpHandleWrapper {
    FileOperationHandle cpp_handle;
    FileOpResultCache cache;

    explicit FileOpHandleWrapper(FileOperationHandle&& h)
        : cpp_handle(std::move(h)) {}
};

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

static EntropyFileOpStatus to_c_status(FileOpStatus s) {
    switch (s) {
        case FileOpStatus::Pending:  return ENTROPY_FILE_OP_PENDING;
        case FileOpStatus::Running:  return ENTROPY_FILE_OP_RUNNING;
        case FileOpStatus::Partial:  return ENTROPY_FILE_OP_PARTIAL;
        case FileOpStatus::Complete: return ENTROPY_FILE_OP_COMPLETE;
        case FileOpStatus::Failed:   return ENTROPY_FILE_OP_FAILED;
        default:                     return ENTROPY_FILE_OP_FAILED;
    }
}

static EntropyFileError to_c_error(FileError e) {
    switch (e) {
        case FileError::None:         return ENTROPY_FILE_ERROR_NONE;
        case FileError::FileNotFound: return ENTROPY_FILE_ERROR_FILE_NOT_FOUND;
        case FileError::AccessDenied: return ENTROPY_FILE_ERROR_ACCESS_DENIED;
        case FileError::DiskFull:     return ENTROPY_FILE_ERROR_DISK_FULL;
        case FileError::InvalidPath:  return ENTROPY_FILE_ERROR_INVALID_PATH;
        case FileError::IOError:      return ENTROPY_FILE_ERROR_IO_ERROR;
        case FileError::NetworkError: return ENTROPY_FILE_ERROR_NETWORK_ERROR;
        case FileError::Timeout:      return ENTROPY_FILE_ERROR_TIMEOUT;
        case FileError::Conflict:     return ENTROPY_FILE_ERROR_CONFLICT;
        case FileError::Unknown:      return ENTROPY_FILE_ERROR_UNKNOWN;
        default:                      return ENTROPY_FILE_ERROR_UNKNOWN;
    }
}

static void cache_metadata(FileOpResultCache& cache, const FileMetadata& meta) {
    std::lock_guard<std::mutex> lock(cache.mutex);

    cache.metadata_path = meta.path;
    cache.c_metadata.path = cache.metadata_path.c_str();
    cache.c_metadata.exists = meta.exists ? ENTROPY_TRUE : ENTROPY_FALSE;
    cache.c_metadata.is_directory = meta.isDirectory ? ENTROPY_TRUE : ENTROPY_FALSE;
    cache.c_metadata.is_regular_file = meta.isRegularFile ? ENTROPY_TRUE : ENTROPY_FALSE;
    cache.c_metadata.is_symlink = meta.isSymlink ? ENTROPY_TRUE : ENTROPY_FALSE;
    cache.c_metadata.size = meta.size;
    cache.c_metadata.readable = meta.readable ? ENTROPY_TRUE : ENTROPY_FALSE;
    cache.c_metadata.writable = meta.writable ? ENTROPY_TRUE : ENTROPY_FALSE;
    cache.c_metadata.executable = meta.executable ? ENTROPY_TRUE : ENTROPY_FALSE;

    if (meta.lastModified.has_value()) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            meta.lastModified->time_since_epoch()).count();
        cache.c_metadata.last_modified_ms = ms;
    } else {
        cache.c_metadata.last_modified_ms = -1;
    }

    if (meta.mimeType.has_value()) {
        cache.metadata_mime = *meta.mimeType;
        cache.c_metadata.mime_type = cache.metadata_mime.c_str();
    } else {
        cache.c_metadata.mime_type = nullptr;
    }

    cache.metadata_cached = true;
}

static void cache_entries(FileOpResultCache& cache, const std::vector<DirectoryEntry>& entries) {
    std::lock_guard<std::mutex> lock(cache.mutex);

    cache.c_entries.resize(entries.size());
    cache.entry_names.resize(entries.size());
    cache.entry_paths.resize(entries.size());
    cache.entry_metadata_paths.resize(entries.size());
    cache.entry_symlink_targets.resize(entries.size());
    cache.entry_mime_types.resize(entries.size());

    for (size_t i = 0; i < entries.size(); i++) {
        const auto& src = entries[i];
        auto& dst = cache.c_entries[i];

        cache.entry_names[i] = src.name;
        cache.entry_paths[i] = src.fullPath;
        dst.name = cache.entry_names[i].c_str();
        dst.full_path = cache.entry_paths[i].c_str();

        // Metadata
        cache.entry_metadata_paths[i] = src.metadata.path;
        dst.metadata.path = cache.entry_metadata_paths[i].c_str();
        dst.metadata.exists = src.metadata.exists ? ENTROPY_TRUE : ENTROPY_FALSE;
        dst.metadata.is_directory = src.metadata.isDirectory ? ENTROPY_TRUE : ENTROPY_FALSE;
        dst.metadata.is_regular_file = src.metadata.isRegularFile ? ENTROPY_TRUE : ENTROPY_FALSE;
        dst.metadata.is_symlink = src.metadata.isSymlink ? ENTROPY_TRUE : ENTROPY_FALSE;
        dst.metadata.size = src.metadata.size;
        dst.metadata.readable = src.metadata.readable ? ENTROPY_TRUE : ENTROPY_FALSE;
        dst.metadata.writable = src.metadata.writable ? ENTROPY_TRUE : ENTROPY_FALSE;
        dst.metadata.executable = src.metadata.executable ? ENTROPY_TRUE : ENTROPY_FALSE;

        if (src.metadata.lastModified.has_value()) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                src.metadata.lastModified->time_since_epoch()).count();
            dst.metadata.last_modified_ms = ms;
        } else {
            dst.metadata.last_modified_ms = -1;
        }

        if (src.metadata.mimeType.has_value()) {
            cache.entry_mime_types[i] = *src.metadata.mimeType;
            dst.metadata.mime_type = cache.entry_mime_types[i].c_str();
        } else {
            dst.metadata.mime_type = nullptr;
        }

        dst.is_symlink = src.isSymlink ? ENTROPY_TRUE : ENTROPY_FALSE;
        if (src.symlinkTarget.has_value()) {
            cache.entry_symlink_targets[i] = *src.symlinkTarget;
            dst.symlink_target = cache.entry_symlink_targets[i].c_str();
        } else {
            dst.symlink_target = nullptr;
        }
    }

    cache.entries_cached = true;
}

static void cache_error(FileOpResultCache& cache, const FileErrorInfo& err) {
    std::lock_guard<std::mutex> lock(cache.mutex);

    cache.c_error.code = to_c_error(err.code);

    cache.error_message = err.message;
    cache.c_error.message = cache.error_message.c_str();

    cache.error_path = err.path;
    cache.c_error.path = cache.error_path.c_str();

    if (err.systemError.has_value()) {
        cache.c_error.system_error_code = err.systemError->value();
        cache.error_category = err.systemError->category().name();
        cache.c_error.system_error_category = cache.error_category.c_str();
    } else {
        cache.c_error.system_error_code = 0;
        cache.c_error.system_error_category = nullptr;
    }

    cache.error_cached = true;
}

/* ============================================================================
 * FileOperationHandle Implementation
 * ============================================================================ */

extern "C" {

entropy_FileOperationHandle entropy_file_operation_handle_clone(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* wrapper = reinterpret_cast<FileOpHandleWrapper*>(handle);
        // Copy the C++ handle (it's value-semantic with shared state)
        auto* clone = new(std::nothrow) FileOpHandleWrapper(FileOperationHandle(wrapper->cpp_handle));
        if (!clone) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        *status = ENTROPY_OK;
        return reinterpret_cast<entropy_FileOperationHandle>(clone);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

void entropy_file_operation_handle_destroy(entropy_FileOperationHandle handle) {
    if (!handle) return;
    auto* wrapper = reinterpret_cast<FileOpHandleWrapper*>(handle);
    delete wrapper;
}

void entropy_file_operation_handle_wait(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
) {
    if (!status) return;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        auto* wrapper = reinterpret_cast<FileOpHandleWrapper*>(handle);
        wrapper->cpp_handle.wait();
        *status = ENTROPY_OK;
    } catch (...) {
        translate_exception(status);
    }
}

EntropyFileOpStatus entropy_file_operation_handle_status(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
) {
    if (!status) return ENTROPY_FILE_OP_FAILED;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_FILE_OP_FAILED;
    }

    try {
        auto* wrapper = reinterpret_cast<FileOpHandleWrapper*>(handle);
        *status = ENTROPY_OK;
        return to_c_status(wrapper->cpp_handle.status());
    } catch (...) {
        translate_exception(status);
        return ENTROPY_FILE_OP_FAILED;
    }
}

const uint8_t* entropy_file_operation_handle_contents_bytes(
    entropy_FileOperationHandle handle,
    size_t* out_size,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle || !out_size) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* wrapper = reinterpret_cast<FileOpHandleWrapper*>(handle);
        auto bytes = wrapper->cpp_handle.contentsBytes();
        if (bytes.empty()) {
            *out_size = 0;
            *status = ENTROPY_OK;
            return nullptr;
        }
        *out_size = bytes.size();
        *status = ENTROPY_OK;
        return reinterpret_cast<const uint8_t*>(bytes.data());
    } catch (...) {
        translate_exception(status);
        *out_size = 0;
        return nullptr;
    }
}

const char* entropy_file_operation_handle_contents_text(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* wrapper = reinterpret_cast<FileOpHandleWrapper*>(handle);
        std::string text = wrapper->cpp_handle.contentsText();
        if (text.empty()) {
            *status = ENTROPY_OK;
            return "";
        }

        // Cache the text in the wrapper to keep it alive
        // Note: contentsText() returns a new string each time, so we need to cache it
        // For now, return the data from contentsBytes converted to char*
        auto bytes = wrapper->cpp_handle.contentsBytes();
        if (bytes.empty()) {
            *status = ENTROPY_OK;
            return "";
        }

        *status = ENTROPY_OK;
        return reinterpret_cast<const char*>(bytes.data());
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

uint64_t entropy_file_operation_handle_bytes_written(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
) {
    if (!status) return 0;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return 0;
    }

    try {
        auto* wrapper = reinterpret_cast<FileOpHandleWrapper*>(handle);
        *status = ENTROPY_OK;
        return wrapper->cpp_handle.bytesWritten();
    } catch (...) {
        translate_exception(status);
        return 0;
    }
}

const EntropyFileMetadata* entropy_file_operation_handle_metadata(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* wrapper = reinterpret_cast<FileOpHandleWrapper*>(handle);
        const auto& meta_opt = wrapper->cpp_handle.metadata();
        if (!meta_opt.has_value()) {
            *status = ENTROPY_OK;
            return nullptr;
        }

        // Cache the metadata
        if (!wrapper->cache.metadata_cached) {
            cache_metadata(wrapper->cache, *meta_opt);
        }

        *status = ENTROPY_OK;
        return &wrapper->cache.c_metadata;
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

const EntropyDirectoryEntry* entropy_file_operation_handle_directory_entries(
    entropy_FileOperationHandle handle,
    size_t* out_count,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle || !out_count) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* wrapper = reinterpret_cast<FileOpHandleWrapper*>(handle);
        const auto& entries = wrapper->cpp_handle.directoryEntries();
        if (entries.empty()) {
            *out_count = 0;
            *status = ENTROPY_OK;
            return nullptr;
        }

        // Cache the entries
        if (!wrapper->cache.entries_cached) {
            cache_entries(wrapper->cache, entries);
        }

        *out_count = wrapper->cache.c_entries.size();
        *status = ENTROPY_OK;
        return wrapper->cache.c_entries.data();
    } catch (...) {
        translate_exception(status);
        *out_count = 0;
        return nullptr;
    }
}

const EntropyFileErrorInfo* entropy_file_operation_handle_error_info(
    entropy_FileOperationHandle handle,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* wrapper = reinterpret_cast<FileOpHandleWrapper*>(handle);
        const auto& err = wrapper->cpp_handle.errorInfo();

        // Cache the error
        if (!wrapper->cache.error_cached) {
            cache_error(wrapper->cache, err);
        }

        *status = ENTROPY_OK;
        return &wrapper->cache.c_error;
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

} // extern "C"

// Internal helper for other C API files to create FileOpHandleWrapper
namespace EntropyEngine::Core::IO {
    extern "C" entropy_FileOperationHandle wrap_file_operation_handle(FileOperationHandle&& handle) {
        auto* wrapper = new(std::nothrow) FileOpHandleWrapper(std::move(handle));
        return reinterpret_cast<entropy_FileOperationHandle>(wrapper);
    }
}
