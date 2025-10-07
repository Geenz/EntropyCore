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

class VirtualFileSystem {
public:
    struct Config { 
        bool serializeWritesPerPath = true;
        size_t maxWriteLocksCached = 1024;  // Maximum number of write locks to cache
        std::chrono::minutes writeLockTimeout{5};  // Timeout for unused write locks
        bool defaultCreateParentDirs = false;       // Default behavior for creating parent directories
    };

    explicit VirtualFileSystem(EntropyEngine::Core::Concurrency::WorkContractGroup* group, Config cfg = {});
    ~VirtualFileSystem();

    // Factory is defined in .cpp to avoid circular includes issues
    FileHandle createFileHandle(std::string path);
    // Ergonomic helpers for value-semantic handle reuse
    FileHandle handle(std::string path) { return createFileHandle(std::move(path)); }
    FileHandle operator()(std::string path) { return createFileHandle(std::move(path)); }
    
    // Streaming convenience
    std::unique_ptr<FileStream> openStream(const std::string& path, StreamOptions options = {});
    std::unique_ptr<BufferedFileStream> openBufferedStream(const std::string& path, size_t bufferSize = 65536, StreamOptions options = {});
    
    // Batch operations
    std::unique_ptr<WriteBatch> createWriteBatch(const std::string& path);

    // File watching
    FileWatch* watchDirectory(const std::string& path, FileWatchCallback callback, const WatchOptions& options = {});
    void unwatchDirectory(FileWatch* watch);

    // Backend management
    void setDefaultBackend(std::shared_ptr<IFileSystemBackend> backend);
    void mountBackend(const std::string& prefix, std::shared_ptr<IFileSystemBackend> backend);
    std::shared_ptr<IFileSystemBackend> findBackend(const std::string& path) const;
    std::shared_ptr<IFileSystemBackend> getDefaultBackend() const {
        std::shared_lock lock(_backendMutex);
        return _defaultBackend;
    }

private:
    using Group = EntropyEngine::Core::Concurrency::WorkContractGroup;
    Group* _group;
    Config _cfg{};

    // LRU cache for write serialization per path
    struct LockEntry {
        std::shared_ptr<std::mutex> mutex;
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
    std::shared_ptr<std::mutex> lockForPath(const std::string& path) const;
    void evictOldLocks(std::chrono::steady_clock::time_point now) const;

    FileOperationHandle submit(std::string path, std::function<void(FileOperationHandle::OpState&, const std::string&)> body) const;
    
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
