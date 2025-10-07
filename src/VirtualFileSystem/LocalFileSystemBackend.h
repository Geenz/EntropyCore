#pragma once
#include "IFileSystemBackend.h"
#include <mutex>
#include <unordered_map>

namespace EntropyEngine::Core::IO {

class LocalFileSystemBackend : public IFileSystemBackend {
public:
    LocalFileSystemBackend();
    ~LocalFileSystemBackend() override = default;
    
    // Core file operations
    FileOperationHandle readFile(const std::string& path, ReadOptions options = {}) override;
    FileOperationHandle writeFile(const std::string& path, std::span<const std::byte> data, WriteOptions options = {}) override;
    FileOperationHandle deleteFile(const std::string& path) override;
    FileOperationHandle createFile(const std::string& path) override;
    
    // Metadata operations
    FileOperationHandle getMetadata(const std::string& path) override;
    bool exists(const std::string& path) override;
    FileOperationHandle getMetadataBatch(const BatchMetadataOptions& options) override;
    
    // Directory operations
    FileOperationHandle createDirectory(const std::string& path) override;
    FileOperationHandle removeDirectory(const std::string& path) override;
    FileOperationHandle listDirectory(const std::string& path, ListDirectoryOptions options = {}) override;
    
    // Stream support
    std::unique_ptr<FileStream> openStream(const std::string& path, StreamOptions options = {}) override;
    
    // Line operations
    FileOperationHandle readLine(const std::string& path, size_t lineNumber) override;
    FileOperationHandle writeLine(const std::string& path, size_t lineNumber, std::string_view line) override;

    // Copy/Move operations (Phase 2)
    FileOperationHandle copyFile(const std::string& src, const std::string& dst, const CopyOptions& options = {}) override;
    FileOperationHandle moveFile(const std::string& src, const std::string& dst, bool overwriteExisting = false) override;
    
    // Backend info
    BackendCapabilities getCapabilities() const override;
    std::string getBackendType() const override { return "LocalFileSystem"; }

    // Backend-aware normalization for identity/locking
    std::string normalizeKey(const std::string& path) const override;
    
private:
    // Helper to get a write lock for a path if needed
    std::shared_ptr<std::mutex> getWriteLock(const std::string& path);
    
    // Submit work to the VFS work group
    FileOperationHandle submitWork(const std::string& path, 
                                  std::function<void(FileOperationHandle::OpState&, const std::string&)> work);
                                  
    // Write lock management (similar to what VFS had)
    mutable std::mutex _lockMapMutex;
    mutable std::unordered_map<std::string, std::shared_ptr<std::mutex>> _writeLocks;
};

} // namespace EntropyEngine::Core::IO