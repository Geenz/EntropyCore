/**
 * @file entropy_write_batch_c.cpp
 * @brief Implementation of WriteBatch C API
 */

#include <new>

#include "VirtualFileSystem/FileOperationHandle.h"
#include "VirtualFileSystem/IFileSystemBackend.h"
#include "VirtualFileSystem/WriteBatch.h"
#include "entropy/entropy_write_batch.h"

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
 * WriteBatch Implementation
 * ============================================================================ */

extern "C" {

void entropy_write_batch_destroy(entropy_WriteBatch batch) {
    if (!batch) return;
    auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
    delete cpp_batch;
}

void entropy_write_batch_write_line(entropy_WriteBatch batch, size_t line_number, const char* content,
                                    EntropyStatus* status) {
    if (!status) return;
    if (!batch || !content) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        cpp_batch->writeLine(line_number, content);
        *status = ENTROPY_OK;
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_write_batch_insert_line(entropy_WriteBatch batch, size_t line_number, const char* content,
                                     EntropyStatus* status) {
    if (!status) return;
    if (!batch || !content) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        cpp_batch->insertLine(line_number, content);
        *status = ENTROPY_OK;
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_write_batch_delete_line(entropy_WriteBatch batch, size_t line_number, EntropyStatus* status) {
    if (!status) return;
    if (!batch) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        cpp_batch->deleteLine(line_number);
        *status = ENTROPY_OK;
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_write_batch_append_line(entropy_WriteBatch batch, const char* content, EntropyStatus* status) {
    if (!status) return;
    if (!batch || !content) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        cpp_batch->appendLine(content);
        *status = ENTROPY_OK;
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_write_batch_replace_all(entropy_WriteBatch batch, const char* content, EntropyStatus* status) {
    if (!status) return;
    if (!batch || !content) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        cpp_batch->replaceAll(content);
        *status = ENTROPY_OK;
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_write_batch_clear(entropy_WriteBatch batch, EntropyStatus* status) {
    if (!status) return;
    if (!batch) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        cpp_batch->clear();
        *status = ENTROPY_OK;
    } catch (...) {
        translate_exception(status);
    }
}

entropy_FileOperationHandle entropy_write_batch_commit(entropy_WriteBatch batch, EntropyStatus* status) {
    if (!status) return nullptr;
    if (!batch) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        FileOperationHandle op = cpp_batch->commit();
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_write_batch_commit_with_options(entropy_WriteBatch batch,
                                                                    const EntropyWriteOptions* options,
                                                                    EntropyStatus* status) {
    if (!status) return nullptr;
    if (!batch || !options) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        WriteOptions wo = to_cpp_write_options(options);
        FileOperationHandle op = cpp_batch->commit(wo);
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_FileOperationHandle entropy_write_batch_preview(entropy_WriteBatch batch, EntropyStatus* status) {
    if (!status) return nullptr;
    if (!batch) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        FileOperationHandle op = cpp_batch->preview();
        *status = ENTROPY_OK;
        return wrap_file_operation_handle(std::move(op));
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

size_t entropy_write_batch_pending_operations(entropy_WriteBatch batch, EntropyStatus* status) {
    if (!status) return 0;
    if (!batch) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return 0;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        *status = ENTROPY_OK;
        return cpp_batch->pendingOperations();
    } catch (...) {
        translate_exception(status);
        return 0;
    }
}

EntropyBool entropy_write_batch_is_empty(entropy_WriteBatch batch, EntropyStatus* status) {
    if (!status) return ENTROPY_FALSE;
    if (!batch) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_FALSE;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        *status = ENTROPY_OK;
        return cpp_batch->empty() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        translate_exception(status);
        return ENTROPY_FALSE;
    }
}

void entropy_write_batch_reset(entropy_WriteBatch batch, EntropyStatus* status) {
    if (!status) return;
    if (!batch) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        cpp_batch->reset();
        *status = ENTROPY_OK;
    } catch (...) {
        translate_exception(status);
    }
}

const char* entropy_write_batch_get_path(entropy_WriteBatch batch, EntropyStatus* status) {
    if (!status) return nullptr;
    if (!batch) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_batch = reinterpret_cast<WriteBatch*>(batch);
        *status = ENTROPY_OK;
        return cpp_batch->getPath().c_str();
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

}  // extern "C"
