#include "VirtualFileSystem.h"
#include "FileOperationHandle.h"
#include "FileHandle.h"
#include "DirectoryHandle.h"
#include "LocalFileSystemBackend.h"
#include "WriteBatch.h"
#include "FileWatchManager.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cassert>

using EntropyEngine::Core::Concurrency::ExecutionType;

namespace EntropyEngine::Core::IO {


// Constructor / Destructor
VirtualFileSystem::VirtualFileSystem(EntropyEngine::Core::Concurrency::WorkContractGroup* group, Config cfg)
    : _group(group)
    , _cfg(cfg)
    , _watchManager(std::make_unique<FileWatchManager>(this)) {
}

VirtualFileSystem::~VirtualFileSystem() {
    // Ensure FileWatchManager is destroyed before WorkContractGroup is potentially destroyed
    _watchManager.reset();
}

std::shared_ptr<IFileSystemBackend> VirtualFileSystem::getDefaultBackend() const {
    {
        std::shared_lock rlock(_backendMutex);
        if (_defaultBackend) return _defaultBackend;
    }
    std::unique_lock wlock(_backendMutex);
    if (!_defaultBackend) {
        auto localBackend = std::make_shared<LocalFileSystemBackend>();
        localBackend->setVirtualFileSystem(const_cast<VirtualFileSystem*>(this));
        const_cast<VirtualFileSystem*>(this)->_defaultBackend = localBackend;
    }
    return _defaultBackend;
}

// VFS submit helper
FileOperationHandle VirtualFileSystem::submit(std::string path, std::function<void(FileOperationHandle::OpState&, const std::string&, const ExecContext&)> body) const {
    auto st = makeState();
    // Set cooperative progress hook so wait() can pump ready work
    st->progress = [grp=_group]() { if (grp) grp->executeAllBackgroundWork(); };

    auto work = [this, st, p=std::move(path), body=std::move(body)]() mutable {
        st->st.store(FileOpStatus::Running, std::memory_order_release);
        try {
            ExecContext ctx{ _group };
            body(*st, p, ctx);
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
        } catch (const std::filesystem::filesystem_error& fe) {
            if (!st->isComplete.load(std::memory_order_acquire)) {
                st->setError(FileError::IOError, fe.what(), p, fe.code());
                st->complete(FileOpStatus::Failed);
            }
            return;
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

FileOperationHandle VirtualFileSystem::submitSerialized(std::string path, std::function<void(FileOperationHandle::OpState&, std::shared_ptr<IFileSystemBackend>, const std::string&, const ExecContext&)> op) const {
    auto backend = findBackend(path);
    auto vfsLock = lockForPath(path);
    auto advTimeout = _cfg.advisoryAcquireTimeout;
    auto policy = _cfg.advisoryFallback;

    return submit(std::move(path), [this, backend, vfsLock, advTimeout, policy, op=std::move(op)](FileOperationHandle::OpState& s, const std::string& p, const ExecContext& ctx) mutable {
        // Acquire backend-specific scope first
        std::unique_ptr<void, void(*)(void*)> scopeToken(nullptr, [](void*){});
        IFileSystemBackend::AcquireWriteScopeResult scopeRes;
        if (backend) {
            IFileSystemBackend::AcquireScopeOptions opts;
            opts.nonBlocking = false;
            if (policy == Config::AdvisoryFallbackPolicy::FallbackWithTimeout || policy == Config::AdvisoryFallbackPolicy::None) {
                opts.timeout = advTimeout;
            }
            scopeRes = backend->acquireWriteScope(p, opts);
            if (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::Acquired) {
                scopeToken = std::move(scopeRes.token);
            }
        }
        std::unique_lock<std::timed_mutex> pathLock;
        if (!scopeToken && vfsLock) {
            bool needFallback = (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::NotSupported) ||
                                (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::Acquired && !scopeToken) ||
                                (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::Busy) ||
                                (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::TimedOut) ||
                                (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::Error);
            if (needFallback) {
                if ((scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::Busy) ||
                    (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::TimedOut) ||
                    (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::Error)) {
                    if (policy == Config::AdvisoryFallbackPolicy::None) {
                        FileError code;
                        if (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::TimedOut) code = FileError::Timeout;
                        else if (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::Busy) code = FileError::Conflict;
                        else code = FileError::IOError;
                        s.setError(code, scopeRes.message.empty() ? std::string("Backend write scope unavailable") : scopeRes.message, p, scopeRes.errorCode);
                        s.complete(FileOpStatus::Failed);
                        return;
                    }
                }
                if (policy == Config::AdvisoryFallbackPolicy::FallbackWithTimeout) {
                    if (!vfsLock->try_lock_for(advTimeout)) {
                        auto key = backend ? backend->normalizeKey(p) : this->normalizePath(p);
                        auto ms = advTimeout.count();
                        s.setError(FileError::Timeout, std::string("Advisory lock acquisition timed out after ") + std::to_string(ms) + " ms (key=" + key + ")", p);
                        s.complete(FileOpStatus::Failed);
                        return;
                    }
                    pathLock = std::unique_lock<std::timed_mutex>(*vfsLock, std::adopt_lock);
                } else { // FallbackThenWait
                    vfsLock->lock();
                    pathLock = std::unique_lock<std::timed_mutex>(*vfsLock, std::adopt_lock);
                }
            }
        }

        // Execute backend operation inline (caller must not call async backend methods)
        if (!backend) {
            s.setError(FileError::Unknown, "No backend available for path", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        op(s, backend, p, ctx);
        // Debug assertion: serialized op must complete inline
#ifndef NDEBUG
        assert(s.isComplete.load(std::memory_order_acquire) && "submitSerialized op must call complete() inline");
#endif
    });
}

// Factory
FileHandle VirtualFileSystem::createFileHandle(std::string path) {
    // Find the appropriate backend for this path
    auto backend = findBackend(path);
    if (!backend) {
        // Use default backend if no specific mount matches
        backend = getDefaultBackend();
        if (!backend) {
            // Create a default local backend if none exists
            auto localBackend = std::make_shared<LocalFileSystemBackend>();
            localBackend->setVirtualFileSystem(this);
            setDefaultBackend(localBackend);
            backend = localBackend;
        }
    }

    // Create handle with backend
    FileHandle handle(this, std::move(path));
    handle._backend = backend;
    // Capture backend-normalized key for value identity
    handle._normKey = backend ? backend->normalizeKey(handle._meta.path) : normalizePath(handle._meta.path);
    return handle;
}

DirectoryHandle VirtualFileSystem::createDirectoryHandle(std::string path) {
    // Ensure a backend is available (create default local if necessary)
    auto backend = findBackend(path);
    if (!backend) {
        backend = getDefaultBackend();
        if (!backend) {
            auto localBackend = std::make_shared<LocalFileSystemBackend>();
            localBackend->setVirtualFileSystem(this);
            setDefaultBackend(localBackend);
            backend = localBackend;
        }
    }
    DirectoryHandle handle(this, std::move(path));
    handle._backend = backend;
    handle._normKey = backend ? backend->normalizeKey(handle._meta.path) : normalizePath(handle._meta.path);
    return handle;
}

// Backend management
void VirtualFileSystem::setDefaultBackend(std::shared_ptr<IFileSystemBackend> backend) {
    std::unique_lock lock(_backendMutex);
    if (backend) {
        backend->setVirtualFileSystem(this);
    }
    _defaultBackend = backend;
}

void VirtualFileSystem::mountBackend(const std::string& prefix, std::shared_ptr<IFileSystemBackend> backend) {
    std::unique_lock lock(_backendMutex);
    if (backend) {
        backend->setVirtualFileSystem(this);
    }
    _mountedBackends[prefix] = backend;
}

std::shared_ptr<IFileSystemBackend> VirtualFileSystem::findBackend(const std::string& path) const {
    std::shared_lock lock(_backendMutex);

    // Check mounted backends for longest matching prefix
    std::shared_ptr<IFileSystemBackend> bestMatch;
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
            bestMatch = backend;
            longestPrefix = prefixLower.length();
        }
    }
#else
    for (const auto& [prefix, backend] : _mountedBackends) {
        if (path.rfind(prefix, 0) == 0 && prefix.length() > longestPrefix) {
            bestMatch = backend;
            longestPrefix = prefix.length();
        }
    }
#endif

    return bestMatch ? bestMatch : _defaultBackend;
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
std::shared_ptr<std::timed_mutex> VirtualFileSystem::lockForPath(const std::string& path) const {
    if (!_cfg.serializeWritesPerPath) return {};
    
    std::lock_guard lk(_mapMutex);
    auto now = std::chrono::steady_clock::now();
    std::string key;
    if (auto backend = findBackend(path)) {
        key = backend->normalizeKey(path);
    } else {
        key = normalizePath(path);
    }
    
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
    auto m = std::make_shared<std::timed_mutex>();
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
    auto backend = findBackend(path);
    if (!backend) {
        // Ensure default backend exists
        backend = getDefaultBackend();
        if (!backend) {
            auto localBackend = std::make_shared<LocalFileSystemBackend>();
            localBackend->setVirtualFileSystem(this);
            setDefaultBackend(localBackend);
            backend = localBackend;
        }
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

std::unique_ptr<WriteBatch> VirtualFileSystem::createWriteBatch(const std::string& path) {
    return std::make_unique<WriteBatch>(this, path);
}

// File watching
FileWatch* VirtualFileSystem::watchDirectory(const std::string& path, FileWatchCallback callback, const WatchOptions& options) {
    if (!_watchManager) {
        return nullptr;
    }
    return _watchManager->createWatch(path, std::move(callback), options);
}

void VirtualFileSystem::unwatchDirectory(FileWatch* watch) {
    if (_watchManager) {
        _watchManager->destroyWatch(watch);
    }
}

} // namespace EntropyEngine::Core::IO
