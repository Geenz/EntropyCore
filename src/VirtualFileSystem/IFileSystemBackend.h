#pragma once
#include <string>
#include <memory>
#include <span>
#include <optional>
#include <functional>
#include <chrono>
#include <vector>
#include "FileOperationHandle.h"

namespace EntropyEngine::Core::IO {

// Forward declarations
class FileStream;
class VirtualFileSystem;

// Options for various operations
struct ReadOptions {
    uint64_t offset = 0;
    std::optional<size_t> length;
    bool binary = true;
};

struct WriteOptions {
    uint64_t offset = 0;
    bool append = false;
    bool createIfMissing = true;
    bool truncate = false;
    std::optional<bool> createParentDirs;     // per-operation override; nullopt => use VFS default
    std::optional<bool> ensureFinalNewline;   // for whole-file rewrites; nullopt => preserve prior
};

struct StreamOptions {
    enum Mode { Read, Write, ReadWrite };
    Mode mode = Read;
    bool buffered = true;
    size_t bufferSize = 65536;  // 64KB default (Phase 2 optimization)
};

// Backend capabilities
struct BackendCapabilities {
    bool supportsStreaming = true;
    bool supportsRandomAccess = true;
    bool supportsDirectories = true;
    bool supportsMetadata = true;
    bool supportsAtomicWrites = false;
    bool supportsWatching = false;
    bool isRemote = false;
    size_t maxFileSize = SIZE_MAX;
};

// FileMetadata and DirectoryEntry are now defined in FileOperationHandle.h

// Options for directory listing
struct ListDirectoryOptions {
    bool recursive = false;
    bool followSymlinks = true;
    size_t maxDepth = SIZE_MAX;
    std::optional<std::string> globPattern;  // Simple glob matching (*.txt, file?.dat, etc)
    std::function<bool(const DirectoryEntry&)> filter;  // Optional filter callback
};

// Options for batch metadata queries
struct BatchMetadataOptions {
    std::vector<std::string> paths;
    bool includeExtendedAttributes = false;
    std::chrono::seconds cacheTTL = std::chrono::seconds(0);  // 0 = no caching
};

// Options for copy operations
struct CopyOptions {
    bool overwriteExisting = false;
    bool preserveAttributes = true;
    bool useReflink = true;  // Use copy-on-write if available (Linux, APFS)
    std::function<bool(size_t copied, size_t total)> progressCallback;  // Return false to cancel
};

// Options for large file operations with progress
struct ProgressOptions {
    size_t chunkSize = 1024 * 1024;  // 1MB default
    std::function<bool(size_t processed, size_t total)> progressCallback;  // Return false to cancel
};

// Backend interface
class IFileSystemBackend {
public:
    virtual ~IFileSystemBackend() = default;
    
    // Core file operations
    virtual FileOperationHandle readFile(const std::string& path, ReadOptions options = {}) = 0;
    virtual FileOperationHandle writeFile(const std::string& path, std::span<const std::byte> data, WriteOptions options = {}) = 0;
    virtual FileOperationHandle deleteFile(const std::string& path) = 0;
    virtual FileOperationHandle createFile(const std::string& path) = 0;
    
    // Metadata operations
    virtual FileOperationHandle getMetadata(const std::string& path) = 0;
    virtual bool exists(const std::string& path) = 0;

    // Batch metadata query (Phase 2)
    virtual FileOperationHandle getMetadataBatch(const BatchMetadataOptions& options) {
        (void)options;
        return FileOperationHandle{}; // Default: not supported
    }
    
    // Directory operations (optional)
    virtual FileOperationHandle createDirectory(const std::string& path) {
        return FileOperationHandle{}; // Default: not supported
    }
    virtual FileOperationHandle removeDirectory(const std::string& path) {
        return FileOperationHandle{}; // Default: not supported
    }
    virtual FileOperationHandle listDirectory(const std::string& path, ListDirectoryOptions options = {}) {
        (void)options;  // Suppress unused warning
        return FileOperationHandle{}; // Default: not supported
    }
    
    // Stream support
    virtual std::unique_ptr<FileStream> openStream(const std::string& path, StreamOptions options = {}) = 0;
    
    // Line operations
    virtual FileOperationHandle readLine(const std::string& path, size_t lineNumber) = 0;
    virtual FileOperationHandle writeLine(const std::string& path, size_t lineNumber, std::string_view line) = 0;

    // Copy/Move operations (Phase 2)
    virtual FileOperationHandle copyFile(const std::string& src, const std::string& dst, const CopyOptions& options = {}) {
        (void)src; (void)dst; (void)options;
        return FileOperationHandle{}; // Default: not supported
    }

    virtual FileOperationHandle moveFile(const std::string& src, const std::string& dst, bool overwriteExisting = false) {
        (void)src; (void)dst; (void)overwriteExisting;
        return FileOperationHandle{}; // Default: not supported
    }
    
    // Backend info
    virtual BackendCapabilities getCapabilities() const = 0;
    virtual std::string getBackendType() const = 0;

    // Backend-aware path normalization for identity/locking keys
    // Default: pass-through (no normalization).
    // Backends should override to implement their own canonicalization (e.g., case-insensitive on Windows local FS).
    virtual std::string normalizeKey(const std::string& path) const { return path; }
    
    // Set the parent VFS for callbacks
    void setVirtualFileSystem(VirtualFileSystem* vfs) { _vfs = vfs; }
    
protected:
    VirtualFileSystem* _vfs = nullptr;
};

} // namespace EntropyEngine::Core::IO