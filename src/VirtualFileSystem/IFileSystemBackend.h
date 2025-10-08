/**
 * @file IFileSystemBackend.h
 * @brief Backend interface for VirtualFileSystem
 * 
 * Implementations provide concrete file operations (local filesystem, remote stores, etc.).
 * VFS routes operations to a backend selected by path mounting. Backends may override
 * normalizeKey() to define identity/locking keys and can ignore options they do not support.
 */
#pragma once
#include <string>
#include <memory>
#include <span>
#include <optional>
#include <functional>
#include <chrono>
#include <vector>
#include <system_error>
#include "FileOperationHandle.h"

namespace EntropyEngine::Core::IO {

// Forward declarations
class FileStream;
class VirtualFileSystem;

// Options for various operations
/**
 * @brief Options controlling file reads
 * @param offset Starting byte offset (default 0)
 * @param length Optional max bytes to read (reads to EOF if not set)
 * @param binary Open in binary mode (platform newline translation off)
 */
struct ReadOptions {
    uint64_t offset = 0;
    std::optional<size_t> length;
    bool binary = true;
};

/**
 * @brief Options controlling file writes
 * @param offset Starting byte offset (ignored if append=true)
 * @param append Append to end of file
 * @param createIfMissing Create the file if it does not exist
 * @param truncate Truncate file before writing (overrides offset)
 * @param createParentDirs Per-op override to create parent directories
 * @param ensureFinalNewline Force presence/absence of final newline for whole-file rewrites
 */
struct WriteOptions {
    uint64_t offset = 0;
    bool append = false;
    bool createIfMissing = true;
    bool truncate = false;
    std::optional<bool> createParentDirs;     // per-operation override; nullopt => use VFS default
    std::optional<bool> ensureFinalNewline;   // for whole-file rewrites; nullopt => preserve prior
    bool fsync = false;                       // Force data to disk (durability guarantee, Unix/POSIX only)

    // Optional cross-process serialization via sibling lock file (compatible with atomic replace)
    std::optional<bool> useLockFile;          // If true, acquire <path> + lockSuffix as exclusive lock
    std::optional<std::chrono::milliseconds> lockTimeout; // Timeout for acquiring lock (overrides VFS default)
    std::optional<std::string> lockSuffix;    // Suffix for lock file (default from VFS config)
};

/**
 * @brief Options for opening streams
 * @param mode Access mode (read/write/read-write)
 * @param buffered If true, backend may buffer; BufferedFileStream provides explicit buffering
 * @param bufferSize Suggested buffer size when applicable
 */
struct StreamOptions {
    enum Mode { Read, Write, ReadWrite };
    Mode mode = Read;
    bool buffered = true;
    size_t bufferSize = 65536;  // 64KB default (Phase 2 optimization)
};

// Backend capabilities
/**
 * @brief Capabilities advertised by a backend
 * @note VFS may adjust behavior based on these (e.g., advisory locking, atomic writes)
 */
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
/**
 * @brief Options controlling directory listings
 * @param recursive If true, recurse into subdirectories
 * @param followSymlinks Whether to follow symlinks during traversal
 * @param maxDepth Maximum recursion depth
 * @param globPattern Optional simple glob filter (e.g., *.txt)
 * @param filter Optional predicate to include/exclude entries
 * @param includeHidden Include hidden files/directories (default false)
 * @param sortBy Sort order for results (none, name, size, modified)
 * @param maxResults Maximum number of results (for pagination; 0 = unlimited)
 */
struct ListDirectoryOptions {
    enum SortOrder { None, ByName, BySize, ByModifiedTime };

    bool recursive = false;
    bool followSymlinks = true;
    size_t maxDepth = SIZE_MAX;
    std::optional<std::string> globPattern;  // Simple glob matching (*.txt, file?.dat, etc)
    std::function<bool(const DirectoryEntry&)> filter;  // Optional filter callback
    bool includeHidden = false;  // Show hidden files/directories
    SortOrder sortBy = None;     // Sort order for results
    size_t maxResults = 0;       // Max results for pagination (0 = unlimited)
};

// Options for batch metadata queries
/**
 * @brief Options to retrieve metadata for multiple paths
 * @param paths Paths to query
 * @param includeExtendedAttributes Include extended attributes if supported
 * @param cacheTTL Optional cache TTL (0 = no caching)
 */
struct BatchMetadataOptions {
    std::vector<std::string> paths;
    bool includeExtendedAttributes = false;
    std::chrono::seconds cacheTTL = std::chrono::seconds(0);  // 0 = no caching
};

// Options for copy operations
/**
 * @brief Options controlling file copy behavior
 * @param overwriteExisting Replace destination if it exists
 * @param preserveAttributes Preserve timestamps/permissions where supported
 * @param useReflink Use copy-on-write cloning if available
 * @param progressCallback Optional progress callback; return false to cancel
 */
struct CopyOptions {
    bool overwriteExisting = false;
    bool preserveAttributes = true;
    bool useReflink = true;  // Use copy-on-write if available (Linux, APFS)
    std::function<bool(size_t copied, size_t total)> progressCallback;  // Return false to cancel
};

// Options for large file operations with progress
/**
 * @brief Options for chunked operations with progress
 * @param chunkSize Preferred chunk size in bytes
 * @param progressCallback Optional progress callback; return false to cancel
 */
struct ProgressOptions {
    size_t chunkSize = 1024 * 1024;  // 1MB default
    std::function<bool(size_t processed, size_t total)> progressCallback;  // Return false to cancel
};

// Backend interface
class IFileSystemBackend {
public:
    virtual ~IFileSystemBackend() = default;
    
    // Core file operations
    /**
     * @brief Reads file contents
     * @param path Path to read
     * @param options ReadOptions (offset/length, binary)
     * @return Handle whose contents are available after wait()
     *
     * @note **Symlink behavior**: File operations follow symlinks by default on Unix systems.
     * @note Dangling symlinks cause FileError::FileNotFound.
     * @note Use getMetadata() with FileMetadata::isSymlink to detect symlinks before operations.
     * @note Special files (FIFO, device, socket) are rejected with FileError::InvalidPath on Unix.
     */
    virtual FileOperationHandle readFile(const std::string& path, ReadOptions options = {}) = 0;
    /**
     * @brief Writes file contents
     * @param path Target path
     * @param data Bytes to write
     * @param options WriteOptions (append/offset/truncate, parent dirs, final newline, fsync)
     * @return Handle representing the async write
     *
     * @note **Symlink behavior**: Writes follow symlinks and modify the target file.
     * @note Dangling symlinks cause FileError::FileNotFound.
     * @note Special files (FIFO, device, socket) are rejected with FileError::InvalidPath on Unix.
     * @note Set options.fsync=true for durability guarantee (Unix/POSIX only; forces data to disk).
     */
    virtual FileOperationHandle writeFile(const std::string& path, std::span<const std::byte> data, WriteOptions options = {}) = 0;
    /**
     * @brief Deletes a file
     * @param path Target path
     * @return Handle representing the delete operation
     *
     * @note **Symlink behavior**: Deletes the symlink itself, not the target.
     */
    virtual FileOperationHandle deleteFile(const std::string& path) = 0;
    /**
     * @brief Creates an empty file
     * @param path Target path
     * @return Handle representing the create operation
     */
    virtual FileOperationHandle createFile(const std::string& path) = 0;
    
    // Metadata operations
    /**
     * @brief Retrieves metadata for a file
     * @param path Target path
     * @return Handle whose metadata() is populated after wait()
     */
    virtual FileOperationHandle getMetadata(const std::string& path) = 0;
    /**
     * @brief Checks existence of a path
     * @param path Target path
     * @return true if path exists, false otherwise
     */
    virtual bool exists(const std::string& path) = 0;

    // Batch metadata query (Phase 2)
    virtual FileOperationHandle getMetadataBatch(const BatchMetadataOptions& options) {
        (void)options;
        return FileOperationHandle{}; // Default: not supported
    }
    
    // Directory operations (optional)
    /**
     * @brief Creates a directory at the given path
     *
     * **Backend-specific semantics:**
     * - **LocalFileSystemBackend**: Creates all parent directories (like `mkdir -p`). Always succeeds
     *   if directory already exists.
     * - **S3Backend** (future): Creates a 0-byte marker object with "/" suffix for compatibility with
     *   tools expecting directory markers. This is optional; S3 has no native directory concept.
     * - **AzureBlobBackend** (future): With hierarchical namespace (HNS) enabled, creates a true
     *   directory object. Without HNS, creates a 0-byte blob with "/" suffix (like S3).
     * - **WebDAVBackend** (future): Issues HTTP `MKCOL` request to create collection (directory).
     * - **HTTPBackend** (non-WebDAV): Not supported. Returns empty handle.
     *
     * @param path Directory to create
     * @return A FileOperationHandle; status() will be Pending for default impl
     */
    virtual FileOperationHandle createDirectory(const std::string& path) {
        (void)path; // Suppress unused warning
        return FileOperationHandle{}; // Default: not supported
    }
    /**
     * @brief Removes a directory at the given path
     *
     * **Backend-specific semantics:**
     * - **LocalFileSystemBackend**: Removes directory and all contents recursively (like `rm -rf`).
     *   Succeeds even if directory doesn't exist (idempotent).
     * - **S3Backend** (future): Deletes all objects with this prefix. **Warning**: Potentially expensive
     *   for deep hierarchies. May require pagination and multiple requests.
     * - **AzureBlobBackend** (future): Similar to S3. Deletes all blobs matching prefix. With HNS,
     *   can delete directory object itself if empty.
     * - **WebDAVBackend** (future): Issues HTTP `DELETE` on collection. May fail if collection is
     *   non-empty depending on server implementation (RFC 4918 allows but doesn't require recursive delete).
     * - **HTTPBackend** (non-WebDAV): Not supported. Returns empty handle.
     *
     * @param path Directory to remove
     * @return A FileOperationHandle; status() will be Pending for default impl
     */
    virtual FileOperationHandle removeDirectory(const std::string& path) {
        (void)path; // Suppress unused warning
        return FileOperationHandle{}; // Default: not supported
    }
    /**
     * @brief Lists entries in the given directory
     *
     * **Backend-specific semantics:**
     * - **LocalFileSystemBackend**: Native filesystem iteration using `std::filesystem::directory_iterator`.
     *   Supports recursion, glob patterns, hidden file filtering, sorting, and pagination.
     * - **S3Backend** (future): Uses `ListObjectsV2` with delimiter="/" to simulate directory listing.
     *   Recursion requires multiple requests. Pagination is native (continuation tokens).
     * - **AzureBlobBackend** (future): Uses `List Blobs` API with delimiter="/". Similar to S3.
     *   With HNS enabled, can use true hierarchical listing APIs.
     * - **WebDAVBackend** (future): Issues HTTP `PROPFIND` request with `Depth: 1` header for
     *   non-recursive, `Depth: infinity` for recursive. Parses XML response (RFC 4918 multistatus).
     * - **HTTPBackend** (non-WebDAV): Not supported. Returns empty handle.
     *
     * @param path Directory to list
     * @param options Listing options (recursion, filters, sorting, pagination)
     * @return A FileOperationHandle; status() will be Pending for default impl
     */
    virtual FileOperationHandle listDirectory(const std::string& path, ListDirectoryOptions options = {}) {
        (void)path; (void)options;  // Suppress unused warnings
        return FileOperationHandle{}; // Default: not supported
    }
    
    // Stream support
    /**
     * @brief Opens a stream for the given path
     * @param path Target path
     * @param options StreamOptions (mode, buffering)
     * @return Unique pointer to FileStream, or null on failure
     */
    virtual std::unique_ptr<FileStream> openStream(const std::string& path, StreamOptions options = {}) = 0;
    
    // Line operations
    /**
     * @brief Reads a single line by index (0-based)
     * @param path Target file
     * @param lineNumber Line index
     * @return Handle with contentsBytes/Text containing the line
     */
    virtual FileOperationHandle readLine(const std::string& path, size_t lineNumber) = 0;
    /**
     * @brief Replaces a single line by index (0-based)
     * @param path Target file
     * @param lineNumber Line index
     * @param line New content without newline
     * @return Handle representing the write
     */
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

    // Backend-provided write-scope acquisition with explicit status and timeout options
    struct AcquireWriteScopeResult {
        enum class Status {
            Acquired,
            Busy,
            TimedOut,
            NotSupported,
            Error
        } status = Status::NotSupported;
        std::unique_ptr<void, void(*)(void*)> token{nullptr, [](void*){}}; // Opaque RAII token
        std::error_code errorCode{};   // Provider/system error code if any
        std::string message;           // Human-readable context
        std::chrono::milliseconds suggestedBackoff{0}; // Hint for retry/backoff on Busy
    };

    struct AcquireScopeOptions {
        std::optional<std::chrono::milliseconds> timeout; // nullopt => backend default
        bool nonBlocking; // true => do not wait
        AcquireScopeOptions() : timeout(std::nullopt), nonBlocking(false) {}
    };

    // Default implementation: not supported, VFS may fall back to advisory lock
    virtual AcquireWriteScopeResult acquireWriteScope(const std::string& path, AcquireScopeOptions options = {}) {
        (void)path; (void)options;
        return AcquireWriteScopeResult{}; // NotSupported by default
    }

    
    // Set the parent VFS for callbacks
    void setVirtualFileSystem(VirtualFileSystem* vfs) { _vfs = vfs; }
    
protected:
    VirtualFileSystem* _vfs = nullptr;
};

} // namespace EntropyEngine::Core::IO