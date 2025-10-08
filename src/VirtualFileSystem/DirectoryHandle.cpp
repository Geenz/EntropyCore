/**
 * @file DirectoryHandle.cpp
 * @brief Implementation of DirectoryHandle
 */
#include "DirectoryHandle.h"
#include "VirtualFileSystem.h"
#include <filesystem>

namespace EntropyEngine::Core::IO {

DirectoryHandle::DirectoryHandle(VirtualFileSystem* vfs, std::string path)
    : _vfs(vfs) {

    // Find the appropriate backend for this path
    _backend = vfs->findBackend(path);
    if (!_backend) {
        _backend = vfs->getDefaultBackend();
    }

    // Initialize metadata
    _meta.path = path;

    // Extract parent directory and name
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        _meta.directory = p.parent_path().string();
    }
    if (p.has_filename()) {
        _meta.name = p.filename().string();
    }

    // Check existence
    std::error_code ec;
    _meta.exists = std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec);

    // Capture backend-normalized key for value identity
    _normKey = _backend ? _backend->normalizeKey(_meta.path) : _meta.path;
}

FileOperationHandle DirectoryHandle::create(bool createParents) const {
    if (!_backend) {
        return FileOperationHandle::immediate(FileOpStatus::Failed);
    }

    // Use backend's createDirectory, which should handle createParents appropriately
    // For LocalFileSystemBackend, this calls std::filesystem::create_directories (always creates parents)
    (void)createParents; // Currently not used - backend decides behavior
    return _backend->createDirectory(_meta.path);
}

FileOperationHandle DirectoryHandle::remove(bool recursive) const {
    if (!_backend) {
        return FileOperationHandle::immediate(FileOpStatus::Failed);
    }

    // Backend's removeDirectory should handle recursive flag appropriately
    // For LocalFileSystemBackend, this calls std::filesystem::remove_all (always recursive)
    (void)recursive; // Currently not used - backend decides behavior
    return _backend->removeDirectory(_meta.path);
}

FileOperationHandle DirectoryHandle::list(const ListDirectoryOptions& options) const {
    if (!_backend) {
        return FileOperationHandle::immediate(FileOpStatus::Failed);
    }

    return _backend->listDirectory(_meta.path, options);
}

FileOperationHandle DirectoryHandle::getMetadata() const {
    if (!_backend) {
        return FileOperationHandle::immediate(FileOpStatus::Failed);
    }

    return _backend->getMetadata(_meta.path);
}

} // namespace EntropyEngine::Core::IO
