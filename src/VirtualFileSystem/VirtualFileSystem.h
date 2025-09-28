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

namespace EntropyEngine::Core::IO {

class VirtualFileSystem {
public:
    struct Config { 
        bool serializeWritesPerPath = true;
        size_t maxWriteLocksCached = 1024;  // Maximum number of write locks to cache
        std::chrono::minutes writeLockTimeout{5};  // Timeout for unused write locks
    };

    explicit VirtualFileSystem(EntropyEngine::Core::Concurrency::WorkContractGroup* group, Config cfg = {})
        : _group(group), _cfg(cfg) {}

    // Factory is defined in .cpp to avoid circular includes issues
    FileHandle createFileHandle(std::string path);
    
    // Backend management
    void setDefaultBackend(std::unique_ptr<IFileSystemBackend> backend);
    void mountBackend(const std::string& prefix, std::unique_ptr<IFileSystemBackend> backend);
    IFileSystemBackend* findBackend(const std::string& path) const;
    IFileSystemBackend* getDefaultBackend() const { return _defaultBackend.get(); }

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
    
    // Backend storage
    std::unique_ptr<IFileSystemBackend> _defaultBackend;
    std::unordered_map<std::string, std::unique_ptr<IFileSystemBackend>> _mountedBackends;
    mutable std::shared_mutex _backendMutex;

    friend class FileHandle;
    friend class LocalFileSystemBackend;
};

} // namespace EntropyEngine::Core::IO
