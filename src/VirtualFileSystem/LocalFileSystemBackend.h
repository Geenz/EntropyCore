#pragma once
#include <mutex>
#include <unordered_map>

#include "IFileSystemBackend.h"

namespace EntropyEngine::Core::IO
{

class LocalFileSystemBackend : public IFileSystemBackend
{
public:
    LocalFileSystemBackend();
    ~LocalFileSystemBackend() override = default;

    // EntropyObject overrides
    const char* className() const noexcept override {
        return "LocalFileSystemBackend";
    }

    // Core file operations
    FileOperationHandle readFile(const std::string& path, ReadOptions options = {}) override;
    FileOperationHandle writeFile(const std::string& path, std::span<const uint8_t> data,
                                  WriteOptions options = {}) override;
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
    FileOperationHandle copyFile(const std::string& src, const std::string& dst,
                                 const CopyOptions& options = {}) override;
    FileOperationHandle moveFile(const std::string& src, const std::string& dst,
                                 bool overwriteExisting = false) override;

    // Backend info
    BackendCapabilities getCapabilities() const override;
    std::string getBackendType() const override {
        return "LocalFileSystem";
    }

    // Backend-aware normalization for identity/locking
    std::string normalizeKey(const std::string& path) const override;

    // Cross-process file locking (Unix/POSIX only)
    AcquireWriteScopeResult acquireWriteScope(const std::string& path, AcquireScopeOptions options = {}) override;

    // Synchronous operations for use by VFS submitSerialized (these execute inline, no async work)
    void doWriteFile(FileOperationHandle::OpState& s, const std::string& path, std::span<const uint8_t> data,
                     WriteOptions options);
    void doDeleteFile(FileOperationHandle::OpState& s, const std::string& path);
    void doCreateFile(FileOperationHandle::OpState& s, const std::string& path);
    void doWriteLine(FileOperationHandle::OpState& s, const std::string& path, size_t lineNumber,
                     std::string_view line);

private:
    // Submit work to the VFS work group
    FileOperationHandle submitWork(const std::string& path,
                                   std::function<void(FileOperationHandle::OpState&, const std::string&)> work);

    // Context-aware submit, used when called from VFS serialized paths
    FileOperationHandle submitWork(
        const std::string& path,
        std::function<void(FileOperationHandle::OpState&, const std::string&, const ExecContext&)> work,
        const ExecContext& ctx);

    // No internal write lock map; serialization handled by VFS policy
};

}  // namespace EntropyEngine::Core::IO
