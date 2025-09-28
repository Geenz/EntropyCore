#pragma once
#include <string>
#include <memory>
#include <span>
#include <optional>
#include <functional>
#include <chrono>
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
};

struct StreamOptions {
    enum Mode { Read, Write, ReadWrite };
    Mode mode = Read;
    bool buffered = true;
    size_t bufferSize = 8192;
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

// File metadata
struct FileMetadata {
    std::string path;
    bool exists = false;
    bool isDirectory = false;
    bool isRegularFile = false;
    uintmax_t size = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    std::optional<std::chrono::system_clock::time_point> lastModified;
    std::optional<std::string> mimeType;
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
    
    // Directory operations (optional)
    virtual FileOperationHandle createDirectory(const std::string& path) { 
        return FileOperationHandle{}; // Default: not supported
    }
    virtual FileOperationHandle removeDirectory(const std::string& path) { 
        return FileOperationHandle{}; // Default: not supported
    }
    virtual FileOperationHandle listDirectory(const std::string& path) { 
        return FileOperationHandle{}; // Default: not supported
    }
    
    // Stream support
    virtual std::unique_ptr<FileStream> openStream(const std::string& path, StreamOptions options = {}) = 0;
    
    // Line operations
    virtual FileOperationHandle readLine(const std::string& path, size_t lineNumber) = 0;
    virtual FileOperationHandle writeLine(const std::string& path, size_t lineNumber, std::string_view line) = 0;
    
    // Backend info
    virtual BackendCapabilities getCapabilities() const = 0;
    virtual std::string getBackendType() const = 0;
    
    // Set the parent VFS for callbacks
    void setVirtualFileSystem(VirtualFileSystem* vfs) { _vfs = vfs; }
    
protected:
    VirtualFileSystem* _vfs = nullptr;
};

} // namespace EntropyEngine::Core::IO