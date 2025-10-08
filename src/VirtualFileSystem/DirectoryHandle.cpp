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
    // Do not resolve or attach a backend here; DirectoryHandle is a dumb value handle.
    // Backend attachment and normalized key computation are performed by VirtualFileSystem::createDirectoryHandle.

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

    // Check existence at construction time (best-effort)
    std::error_code ec;
    _meta.exists = std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec);

    // Leave _backend null and _normKey empty until VFS factory sets them.
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
