#include "VirtualFileSystem.h"
#include "FileOperationHandle.h"
#include "FileHandle.h"
#include "LocalFileSystemBackend.h"
#include <filesystem>
#include <algorithm>
#include <cctype>

using EntropyEngine::Core::Concurrency::ExecutionType;

namespace EntropyEngine::Core::IO {

// VFS submit helper
FileOperationHandle VirtualFileSystem::submit(std::string path, std::function<void(FileOperationHandle::OpState&, const std::string&)> body) const {
    auto st = makeState();
    auto work = [st, p=std::move(path), body=std::move(body)]() mutable {
        st->st.store(FileOpStatus::Running, std::memory_order_release);
        try {
            body(*st, p);
        } catch (...) {
            st->setError(FileError::Unknown, "Unknown error occurred during file operation");
            st->complete(FileOpStatus::Failed);
            return;
        }
    };
    auto handle = _group->createContract(std::move(work), ExecutionType::AnyThread);
    handle.schedule();
    return FileOperationHandle{std::move(st)};
}

// Factory
FileHandle VirtualFileSystem::createFileHandle(std::string path) {
    // Find the appropriate backend for this path
    auto* backend = findBackend(path);
    if (!backend) {
        // Use default backend if no specific mount matches
        backend = getDefaultBackend();
        if (!backend) {
            // Create a default local backend if none exists
            auto localBackend = std::make_unique<LocalFileSystemBackend>();
            localBackend->setVirtualFileSystem(this);
            backend = localBackend.get();
            setDefaultBackend(std::move(localBackend));
        }
    }
    
    // Create handle with backend
    FileHandle handle(this, std::move(path));
    handle._backend = backend;
    return handle;
}

// Backend management
void VirtualFileSystem::setDefaultBackend(std::unique_ptr<IFileSystemBackend> backend) {
    std::unique_lock lock(_backendMutex);
    if (backend) {
        backend->setVirtualFileSystem(this);
    }
    _defaultBackend = std::move(backend);
}

void VirtualFileSystem::mountBackend(const std::string& prefix, std::unique_ptr<IFileSystemBackend> backend) {
    std::unique_lock lock(_backendMutex);
    if (backend) {
        backend->setVirtualFileSystem(this);
    }
    _mountedBackends[prefix] = std::move(backend);
}

IFileSystemBackend* VirtualFileSystem::findBackend(const std::string& path) const {
    std::shared_lock lock(_backendMutex);
    
    // Check mounted backends for longest matching prefix
    IFileSystemBackend* bestMatch = nullptr;
    size_t longestPrefix = 0;
    
    for (const auto& [prefix, backend] : _mountedBackends) {
        if (path.starts_with(prefix) && prefix.length() > longestPrefix) {
            bestMatch = backend.get();
            longestPrefix = prefix.length();
        }
    }
    
    return bestMatch ? bestMatch : _defaultBackend.get();
}

// Path normalization for consistent lock keys
std::string VirtualFileSystem::normalizePath(const std::string& path) const {
    std::error_code ec;
    auto p = std::filesystem::path(path);
    auto canon = std::filesystem::weakly_canonical(p, ec);
    std::string s = (ec ? p.lexically_normal().string() : canon.string());
#if defined(_WIN32)
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
#endif
    return s;
}

// Lock management for write serialization
std::shared_ptr<std::mutex> VirtualFileSystem::lockForPath(const std::string& path) const {
    if (!_cfg.serializeWritesPerPath) return {};
    
    std::lock_guard lk(_mapMutex);
    auto now = std::chrono::steady_clock::now();
    const std::string key = normalizePath(path);
    
    // Check if path exists in cache
    auto it = _writeLocks.find(key);
    if (it != _writeLocks.end()) {
        // Update access time and move to front of LRU list
        it->second.lastAccess = now;
        _lruList.erase(it->second.lruIt);
        _lruList.push_front(key);
        it->second.lruIt = _lruList.begin();
        return it->second.mutex;
    }
    
    // Evict old entries if cache is full
    if (_writeLocks.size() >= _cfg.maxWriteLocksCached) {
        evictOldLocks(now);
    }
    
    // Create new lock entry
    auto m = std::make_shared<std::mutex>();
    _lruList.push_front(key);
    LockEntry entry{m, now, _lruList.begin()};
    _writeLocks.emplace(key, std::move(entry));
    return m;
}

// LRU eviction of old locks
void VirtualFileSystem::evictOldLocks(std::chrono::steady_clock::time_point now) const {
    // Remove entries that haven't been used recently
    auto cutoff = now - _cfg.writeLockTimeout;
    
    // Start from the end of LRU list (least recently used)
    while (!_lruList.empty() && _writeLocks.size() >= _cfg.maxWriteLocksCached) {
        const auto& path = _lruList.back();
        auto it = _writeLocks.find(path);
        if (it != _writeLocks.end() && it->second.lastAccess < cutoff) {
            _writeLocks.erase(it);
            _lruList.pop_back();
        } else if (_writeLocks.size() >= _cfg.maxWriteLocksCached) {
            // Force evict LRU entry even if not timed out
            _writeLocks.erase(path);
            _lruList.pop_back();
        } else {
            break;
        }
    }
}

} // namespace EntropyEngine::Core::IO
