/**
 * @file entropy_virtual_file_system_c.cpp
 * @brief Implementation of VirtualFileSystem C API
 */

#include <chrono>
#include <new>
#include <string>

#include "Concurrency/WorkContractGroup.h"
#include "VirtualFileSystem/DirectoryHandle.h"
#include "VirtualFileSystem/FileHandle.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/WriteBatch.h"
#include "entropy/entropy_virtual_file_system.h"

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::IO;
using namespace EntropyEngine::Core::Concurrency;

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

static VirtualFileSystem::Config to_cpp_config(const EntropyVFSConfig* c) {
    VirtualFileSystem::Config cfg;
    cfg.serializeWritesPerPath = c->serialize_writes_per_path != ENTROPY_FALSE;
    cfg.maxWriteLocksCached = c->max_write_locks_cached;
    cfg.writeLockTimeout = std::chrono::minutes(c->write_lock_timeout_minutes);
    cfg.defaultCreateParentDirs = c->default_create_parent_dirs != ENTROPY_FALSE;
    cfg.advisoryAcquireTimeout = std::chrono::milliseconds(c->advisory_acquire_timeout_ms);
    cfg.defaultUseLockFile = c->default_use_lock_file != ENTROPY_FALSE;
    cfg.lockAcquireTimeout = std::chrono::milliseconds(c->lock_acquire_timeout_ms);
    if (c->lock_suffix) {
        cfg.lockSuffix = c->lock_suffix;
    }
    return cfg;
}

/* ============================================================================
 * VirtualFileSystem Implementation
 * ============================================================================ */

extern "C" {

entropy_VirtualFileSystem entropy_vfs_create(entropy_WorkContractGroup group, EntropyStatus* status) {
    if (!status) return nullptr;
    if (!group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_group = reinterpret_cast<WorkContractGroup*>(group);
        auto* vfs = new (std::nothrow) VirtualFileSystem(cpp_group);
        if (!vfs) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        *status = ENTROPY_OK;
        return reinterpret_cast<entropy_VirtualFileSystem>(vfs);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_VirtualFileSystem entropy_vfs_create_with_config(entropy_WorkContractGroup group,
                                                         const EntropyVFSConfig* config, EntropyStatus* status) {
    if (!status) return nullptr;
    if (!group || !config) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_group = reinterpret_cast<WorkContractGroup*>(group);
        auto cpp_config = to_cpp_config(config);
        auto* vfs = new (std::nothrow) VirtualFileSystem(cpp_group, cpp_config);
        if (!vfs) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        *status = ENTROPY_OK;
        return reinterpret_cast<entropy_VirtualFileSystem>(vfs);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

void entropy_vfs_destroy(entropy_VirtualFileSystem vfs) {
    if (!vfs) return;
    auto* cpp_vfs = reinterpret_cast<VirtualFileSystem*>(vfs);
    delete cpp_vfs;
}

entropy_FileHandle entropy_vfs_create_file_handle(entropy_VirtualFileSystem vfs, const char* path,
                                                  EntropyStatus* status) {
    if (!status) return nullptr;
    if (!vfs || !path) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_vfs = reinterpret_cast<VirtualFileSystem*>(vfs);
        auto cpp_handle = cpp_vfs->createFileHandle(path);

        // FileHandle is value-semantic, so we need to allocate on heap
        auto* handle = new (std::nothrow) FileHandle(std::move(cpp_handle));
        if (!handle) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        *status = ENTROPY_OK;
        return reinterpret_cast<entropy_FileHandle>(handle);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_DirectoryHandle entropy_vfs_create_directory_handle(entropy_VirtualFileSystem vfs, const char* path,
                                                            EntropyStatus* status) {
    if (!status) return nullptr;
    if (!vfs || !path) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_vfs = reinterpret_cast<VirtualFileSystem*>(vfs);
        auto cpp_handle = cpp_vfs->createDirectoryHandle(path);

        // DirectoryHandle is value-semantic, so allocate on heap
        auto* handle = new (std::nothrow) DirectoryHandle(std::move(cpp_handle));
        if (!handle) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        *status = ENTROPY_OK;
        return reinterpret_cast<entropy_DirectoryHandle>(handle);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_WriteBatch entropy_vfs_create_write_batch(entropy_VirtualFileSystem vfs, const char* path,
                                                  EntropyStatus* status) {
    if (!status) return nullptr;
    if (!vfs || !path) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        auto* cpp_vfs = reinterpret_cast<VirtualFileSystem*>(vfs);
        auto cpp_batch = cpp_vfs->createWriteBatch(path);

        // WriteBatch is returned as unique_ptr, so release and transfer ownership
        auto* batch = cpp_batch.release();
        if (!batch) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        *status = ENTROPY_OK;
        return reinterpret_cast<entropy_WriteBatch>(batch);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

}  // extern "C"
