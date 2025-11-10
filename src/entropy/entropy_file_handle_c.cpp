/**
 * @file entropy_file_handle_c.cpp
 * @brief Implementation of FileHandle C API
 */

#include "entropy/entropy_file_handle.h"
#include "VirtualFileSystem/FileHandle.h"
#include "VirtualFileSystem/FileOperationHandle.h"
#include "VirtualFileSystem/IFileSystemBackend.h"
#include <new>
#include <string>
#include <span>
#include <optional>

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

static WriteOptions to_cpp_write_options(const EntropyWriteOptions* opts) {
    WriteOptions wo;
    wo.offset = opts->offset;
    wo.append = opts->append != ENTROPY_FALSE;
    wo.createIfMissing = opts->create_if_missing != ENTROPY_FALSE;
    wo.truncate = opts->truncate != ENTROPY_FALSE;
    wo.fsync = opts->fsync != ENTROPY_FALSE;

    if (opts->create_parent_dirs >= 0) {
        wo.createParentDirs = opts->create_parent_dirs != 0;
    }

    if (opts->ensure_final_newline >= 0) {
        wo.ensureFinalNewline = opts->ensure_final_newline != 0;
    }

    if (opts->use_lock_file >= 0) {
        wo.useLockFile = opts->use_lock_file != 0;
    }

    if (opts->lock_timeout_ms > 0) {
        wo.lockTimeout = std::chrono::milliseconds(opts->lock_timeout_ms);
    }

    if (opts->lock_suffix) {
        wo.lockSuffix = opts->lock_suffix;
    }

    return wo;
}

/* ============================================================================
 * FileHandle Implementation
 * ============================================================================ */

extern "C" {

entropy_FileHandle entropy_file_handle_clone(
    entropy_FileHandle handle,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        auto* clone = new(std::nothrow) FileHandle(*cpp_handle);
        if (!clone) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        *status = ENTROPY_OK;
        return reinterpret_cast<entropy_FileHandle>(clone);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

void entropy_file_handle_destroy(entropy_FileHandle handle) {
    if (!handle) return;
    auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
    delete cpp_handle;
}

entropy_FileOperationHandle entropy_file_handle_read_all(
    entropy_FileHandle handle,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        FileOperationHandle op = cpp_handle->readAll();
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_file_handle_read_range(
    entropy_FileHandle handle,
    uint64_t offset,
    size_t length,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        FileOperationHandle op = cpp_handle->readRange(offset, length);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_file_handle_read_line(
    entropy_FileHandle handle,
    size_t line_number,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        FileOperationHandle op = cpp_handle->readLine(line_number);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_file_handle_write_all_text(
    entropy_FileHandle handle,
    const char* text,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle || !text) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        FileOperationHandle op = cpp_handle->writeAll(text);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_file_handle_write_all_text_with_options(
    entropy_FileHandle handle,
    const char* text,
    const EntropyWriteOptions* options,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle || !text || !options) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        WriteOptions wo = to_cpp_write_options(options);
        FileOperationHandle op = cpp_handle->writeAll(text, wo);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_file_handle_write_all_bytes(
    entropy_FileHandle handle,
    const uint8_t* bytes,
    size_t length,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle || !bytes) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        std::span<const uint8_t> data(bytes, length);
        FileOperationHandle op = cpp_handle->writeAll(data);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_file_handle_write_all_bytes_with_options(
    entropy_FileHandle handle,
    const uint8_t* bytes,
    size_t length,
    const EntropyWriteOptions* options,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle || !bytes || !options) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        std::span<const uint8_t> data(bytes, length);
        WriteOptions wo = to_cpp_write_options(options);
        FileOperationHandle op = cpp_handle->writeAll(data, wo);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_file_handle_write_range(
    entropy_FileHandle handle,
    uint64_t offset,
    const uint8_t* bytes,
    size_t length,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle || !bytes) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        std::span<const uint8_t> data(bytes, length);
        FileOperationHandle op = cpp_handle->writeRange(offset, data);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_file_handle_write_line(
    entropy_FileHandle handle,
    size_t line_number,
    const char* line,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle || !line) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        FileOperationHandle op = cpp_handle->writeLine(line_number, line);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_file_handle_create_empty(
    entropy_FileHandle handle,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        FileOperationHandle op = cpp_handle->createEmpty();
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_file_handle_remove(
    entropy_FileHandle handle,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        FileOperationHandle op = cpp_handle->remove();
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

const char* entropy_file_handle_normalized_key(
    entropy_FileHandle handle,
    EntropyStatus* status
) {
    if (!status) return nullptr;
    if (!handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_handle = reinterpret_cast<FileHandle*>(handle);
        *status = ENTROPY_OK;
        return cpp_handle->normalizedKey().c_str();
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

} // extern "C"
