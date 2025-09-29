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
            // Ensure complete() was called - if not, call it with success
            // This prevents hanging if body forgets to call complete()
            if (!st->isComplete.load(std::memory_order_acquire)) {
                // Body didn't call complete() - do it now
                // If status is still Running, mark as Complete
                auto expected = FileOpStatus::Running;
                if (st->st.compare_exchange_strong(expected, FileOpStatus::Complete)) {
                    st->complete(FileOpStatus::Complete);
                }
            }
        } catch (...) {
            // Only set error if not already complete
            if (!st->isComplete.load(std::memory_order_acquire)) {
                st->setError(FileError::Unknown, "Unknown error occurred during file operation");
                st->complete(FileOpStatus::Failed);
            }
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

#if defined(_WIN32)
    // Case-insensitive prefix matching on Windows
    auto toLower = [](std::string s){
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string pathLower = toLower(path);
    for (const auto& [prefix, backend] : _mountedBackends) {
        const std::string prefixLower = toLower(prefix);
        if (pathLower.rfind(prefixLower, 0) == 0 && prefixLower.length() > longestPrefix) {
            bestMatch = backend.get();
            longestPrefix = prefixLower.length();
        }
    }
#else
    for (const auto& [prefix, backend] : _mountedBackends) {
        if (path.rfind(prefix, 0) == 0 && prefix.length() > longestPrefix) {
            bestMatch = backend.get();
            longestPrefix = prefix.length();
        }
    }
#endif
    
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

std::unique_ptr<FileStream> VirtualFileSystem::openStream(const std::string& path, StreamOptions options) {
    auto* backend = findBackend(path);
    if (!backend) {
        // Ensure default backend exists
        auto def = getDefaultBackend();
        if (!def) {
            auto localBackend = std::make_unique<LocalFileSystemBackend>();
            localBackend->setVirtualFileSystem(this);
            def = localBackend.get();
            setDefaultBackend(std::move(localBackend));
        }
        backend = def;
    }
    return backend->openStream(path, options);
}

std::unique_ptr<BufferedFileStream> VirtualFileSystem::openBufferedStream(const std::string& path, size_t bufferSize, StreamOptions options) {
    // Force unbuffered inner; buffering is handled by wrapper
    options.buffered = false;
    auto inner = openStream(path, options);
    if (!inner) return {};
    return std::make_unique<BufferedFileStream>(std::move(inner), bufferSize);
}

} // namespace EntropyEngine::Core::IO
