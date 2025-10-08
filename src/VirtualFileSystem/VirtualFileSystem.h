/**
 * @file VirtualFileSystem.h
 * @brief High-level facade for file operations over pluggable backends
 * 
 * VirtualFileSystem (VFS) routes file operations to a selected backend (local filesystem by default)
 * and provides ergonomic helpers: value-semantic FileHandle creation, advisory per-path write
 * serialization, batching, and file watching. Use with a WorkContractGroup; operations are executed
 * asynchronously and can be waited on. See Examples/VirtualFileSystemExample.cpp for end-to-end usage.
 */
#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <memory>
#include <functional>
#include <chrono>
#include <list>
#include <filesystem>
#include <algorithm>
#include "../Concurrency/WorkContractGroup.h"
#include "FileOperationHandle.h"
#include "FileHandle.h"
#include "IFileSystemBackend.h"
#include "FileWatch.h"

namespace EntropyEngine::Core::IO {

class FileStream; // fwd
class BufferedFileStream; // fwd
class WriteBatch; // fwd
class FileWatchManager; // fwd
class FileWatch; // fwd
class DirectoryHandle; // fwd

class VirtualFileSystem {
public:
    struct Config { 
        bool serializeWritesPerPath;
        size_t maxWriteLocksCached;  // Maximum number of write locks to cache
        std::chrono::minutes writeLockTimeout;  // Timeout for unused write locks
        bool defaultCreateParentDirs;       // Default behavior for creating parent directories

        // Advisory locking policy (in-process fallback)
        std::chrono::milliseconds advisoryAcquireTimeout; // 5s default
        enum class AdvisoryFallbackPolicy { None, FallbackThenWait, FallbackWithTimeout };
        AdvisoryFallbackPolicy advisoryFallback;

        // Cross-process lock-file serialization (optional)
        bool defaultUseLockFile;                                    // default: false
        std::chrono::milliseconds lockAcquireTimeout;               // default: 5s
        std::string lockSuffix;                                     // default: ".lock"

        Config()
            : serializeWritesPerPath(true)
            , maxWriteLocksCached(1024)
            , writeLockTimeout(std::chrono::minutes(5))
            , defaultCreateParentDirs(false)
            , advisoryAcquireTimeout(std::chrono::milliseconds(5000))
            , advisoryFallback(AdvisoryFallbackPolicy::FallbackThenWait)
            , defaultUseLockFile(false)
            , lockAcquireTimeout(std::chrono::milliseconds(5000))
            , lockSuffix(".lock") {}
    };

    explicit VirtualFileSystem(EntropyEngine::Core::Concurrency::WorkContractGroup* group, Config cfg = {});
    ~VirtualFileSystem();

    // Factory is defined in .cpp to avoid circular includes issues
    /**
     * @brief Creates a value-semantic handle for the given path
     * 
     * Routes the path to the appropriate backend (mounted or default). The handle is copyable and
     * caches a backend-normalized identity key for equality/locking purposes.
     * @param path Target path
     * @return FileHandle bound to this VFS
     */
    FileHandle createFileHandle(std::string path);
    // Ergonomic helpers for value-semantic handle reuse
    /**
     * @brief Shorthand for createFileHandle(path)
     */
    FileHandle handle(std::string path) { return createFileHandle(std::move(path)); }
    /**
     * @brief Functor shorthand for createFileHandle(path)
     */
    FileHandle operator()(std::string path) { return createFileHandle(std::move(path)); }

    /**
     * @brief Creates a value-semantic handle for a directory path
     *
     * Routes the path to the appropriate backend (mounted or default). The handle is copyable and
     * caches a backend-normalized identity key for equality purposes.
     * @param path Target directory path
     * @return DirectoryHandle bound to this VFS
     */
    DirectoryHandle createDirectoryHandle(std::string path);
    
    // Streaming convenience
    /**
     * @brief Opens a stream via the routed backend
     * @param path Target file path
     * @param options Stream options (mode, buffering)
     * @return Unique pointer to FileStream, or null on failure
     */
    std::unique_ptr<FileStream> openStream(const std::string& path, StreamOptions options = {});
    /**
     * @brief Opens a buffered stream wrapper
     * @param path Target file path
     * @param bufferSize Buffer size in bytes
     * @param options Base stream options (buffered is ignored; wrapper handles buffering)
     * @return BufferedFileStream unique_ptr, or null on failure
     */
    std::unique_ptr<BufferedFileStream> openBufferedStream(const std::string& path, size_t bufferSize = 65536, StreamOptions options = {});
    
    // Batch operations
    /**
     * @brief Creates a WriteBatch builder for atomic multi-line edits
     * @param path Target file path
     * @return Unique pointer to WriteBatch
     */
    std::unique_ptr<WriteBatch> createWriteBatch(const std::string& path);

    // File watching
    /**
     * @brief Watches a directory and invokes callback on changes
     * @param path Directory to watch
     * @param callback Callback invoked on file events
     * @param options Watch options (recursive, filters, etc.)
     * @return Opaque FileWatch handle, or nullptr if watching not available
     */
    FileWatch* watchDirectory(const std::string& path, FileWatchCallback callback, const WatchOptions& options = {});
    /**
     * @brief Stops watching a directory
     * @param watch Handle returned by watchDirectory
     */
    void unwatchDirectory(FileWatch* watch);

    // Backend management
    /**
     * @brief Sets the default backend used when no mount matches
     * @param backend Backend implementation (shared ownership)
     */
    void setDefaultBackend(std::shared_ptr<IFileSystemBackend> backend);
    /**
     * @brief Mounts a backend at a path prefix (longest-prefix match)
     * @param prefix Path prefix (e.g., "s3://bucket/")
     * @param backend Backend to route to when prefix matches
     */
    void mountBackend(const std::string& prefix, std::shared_ptr<IFileSystemBackend> backend);
    /**
     * @brief Finds the backend that would handle a given path
     * @param path Input path
     * @return Shared pointer to backend (mounted or default), or nullptr if none
     */
    std::shared_ptr<IFileSystemBackend> findBackend(const std::string& path) const;
    /**
     * @brief Gets the current default backend
     * @return Shared pointer to default backend (may be null)
     */
    std::shared_ptr<IFileSystemBackend> getDefaultBackend() const;

private:
    using Group = EntropyEngine::Core::Concurrency::WorkContractGroup;
    Group* _group;
    Config _cfg{};

    // LRU cache for write serialization per path
    struct LockEntry {
        std::shared_ptr<std::timed_mutex> mutex;
        std::chrono::steady_clock::time_point lastAccess;
        std::list<std::string>::iterator lruIt;
    };
    
    mutable std::mutex _mapMutex;
    mutable std::unordered_map<std::string, LockEntry> _writeLocks;
    mutable std::list<std::string> _lruList;  // Most recently used at front

    std::shared_ptr<FileOperationHandle::OpState> makeState() const { return std::make_shared<FileOperationHandle::OpState>(); }

    // Path normalization for lock keys
    std::string normalizePath(const std::string& path) const;
    
    // Lock management
    std::shared_ptr<std::timed_mutex> lockForPath(const std::string& path) const;
    void evictOldLocks(std::chrono::steady_clock::time_point now) const;

    FileOperationHandle submit(std::string path, std::function<void(FileOperationHandle::OpState&, const std::string&, const ExecContext&)> body) const;
    
    // Centralized write-serialization submit used by FileHandle write paths
    /**
     * Executes a serialized write operation under backend scope or VFS advisory lock.
     * Policy:
     * 1) Request backend scope with optional timeout (cfg.advisoryAcquireTimeout).
     * 2) If Acquired: run op inline while holding token.
     * 3) Else apply fallback (cfg.advisoryFallback):
     *    - None → fail early (Busy→Conflict, TimedOut→Timeout, Error→IOError)
     *    - FallbackWithTimeout → try_lock_for(timeout) else Timeout
     *    - FallbackThenWait → lock() and proceed
     * Errors include backend message/systemError when provided.
     * The op must complete inline and call s.complete(). It must NOT schedule nested async work.
     */
    FileOperationHandle submitSerialized(std::string path, std::function<void(FileOperationHandle::OpState&, std::shared_ptr<IFileSystemBackend>, const std::string&, const ExecContext&)> op) const;
    
    // Backend storage (reference-counted for thread-safe lifetime management)
    std::shared_ptr<IFileSystemBackend> _defaultBackend;
    std::unordered_map<std::string, std::shared_ptr<IFileSystemBackend>> _mountedBackends;
    mutable std::shared_mutex _backendMutex;

    // File watching
    std::unique_ptr<FileWatchManager> _watchManager;

    friend class FileHandle;
    friend class LocalFileSystemBackend;
    friend class WriteBatch;
    friend class FileWatchManager;
};

} // namespace EntropyEngine::Core::IO
