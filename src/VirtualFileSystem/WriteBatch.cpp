#include "WriteBatch.h"
#include "VirtualFileSystem.h"
#include "FileHandle.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <random>
#include <map>

#if defined(_WIN32)
static std::string vfs_toWinLongPath(const std::string& path) {
    if (path.rfind("\\\\?\\", 0) == 0) return path;
    if (path.rfind("\\\\", 0) == 0) {
        return std::string("\\\\?\\UNC\\") + path.substr(2);
    }
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
        return std::string("\\\\?\\") + path;
    }
    return path;
}
#endif
#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>     // mkstemp(), close(), fsync()
#include <sys/stat.h>   // stat(), chmod()
#include <sys/file.h>   // flock()
#include <fcntl.h>      // open(), O_*
#endif

namespace EntropyEngine::Core::IO {

WriteBatch::WriteBatch(VirtualFileSystem* vfs, std::string path) 
    : _vfs(vfs), _path(std::move(path)) {
}

WriteBatch& WriteBatch::writeLine(size_t lineNumber, std::string_view content) {
    _operations.push_back({OpType::Write, lineNumber, std::string(content)});
    return *this;
}

WriteBatch& WriteBatch::insertLine(size_t lineNumber, std::string_view content) {
    _operations.push_back({OpType::Insert, lineNumber, std::string(content)});
    return *this;
}

WriteBatch& WriteBatch::deleteLine(size_t lineNumber) {
    _operations.push_back({OpType::Delete, lineNumber, ""});
    return *this;
}

WriteBatch& WriteBatch::appendLine(std::string_view content) {
    _operations.push_back({OpType::Append, 0, std::string(content)});
    return *this;
}

WriteBatch& WriteBatch::writeLines(const std::map<size_t, std::string>& lines) {
    for (const auto& [lineNum, content] : lines) {
        writeLine(lineNum, content);
    }
    return *this;
}

WriteBatch& WriteBatch::replaceAll(std::string_view content) {
    _operations.clear();
    _operations.push_back({OpType::Replace, 0, std::string(content)});
    return *this;
}

WriteBatch& WriteBatch::clear() {
    _operations.clear();
    _operations.push_back({OpType::Clear, 0, ""});
    return *this;
}

WriteBatch& WriteBatch::deleteRange(size_t startLine, size_t endLine) {
    for (size_t i = startLine; i <= endLine; ++i) {
        deleteLine(i);
    }
    return *this;
}

WriteBatch& WriteBatch::insertLines(size_t startLine, const std::vector<std::string>& lines) {
    size_t offset = 0;
    for (const auto& line : lines) {
        insertLine(startLine + offset, line);
        offset++;
    }
    return *this;
}

void WriteBatch::reset() {
    _operations.clear();
}

std::vector<std::string> WriteBatch::applyOperations(const std::vector<std::string>& originalLines) const {
    std::vector<std::string> result = originalLines;

    // Separate operations by type for proper ordering
    std::vector<Operation> replaceOps;
    std::map<size_t, std::string> writeOps;  // Use map to handle sparse writes
    std::vector<Operation> insertOps;
    std::vector<Operation> deleteOps;
    std::vector<std::string> appendOps;
    bool shouldClear = false;

    for (const auto& op : _operations) {
        switch (op.type) {
            case OpType::Clear:
                shouldClear = true;
                break;
            case OpType::Replace:
                replaceOps.push_back(op);
                break;
            case OpType::Write:
                writeOps[op.lineNumber] = op.content;  // Later writes override earlier ones
                break;
            case OpType::Insert:
                insertOps.push_back(op);
                break;
            case OpType::Delete:
                deleteOps.push_back(op);
                break;
            case OpType::Append:
                appendOps.push_back(op.content);
                break;
        }
    }

    // Process Clear
    if (shouldClear) {
        result.clear();
    }

    // Process Replace operations (they clear the file)
    for (const auto& op : replaceOps) {
        result.clear();
        std::istringstream iss(op.content);
        std::string line;
        while (std::getline(iss, line)) {
            result.push_back(line);
        }
    }

    // Process Delete operations (highest index first to avoid shifting issues)
    std::sort(deleteOps.begin(), deleteOps.end(),
        [](const Operation& a, const Operation& b) { return a.lineNumber > b.lineNumber; });
    for (const auto& op : deleteOps) {
        if (op.lineNumber < result.size()) {
            result.erase(result.begin() + op.lineNumber);
        }
    }

    // Process Insert operations BEFORE writes (highest index first to avoid shifting issues)
    std::sort(insertOps.begin(), insertOps.end(),
        [](const Operation& a, const Operation& b) { return a.lineNumber > b.lineNumber; });
    for (const auto& op : insertOps) {
        if (op.lineNumber > result.size()) {
            result.resize(op.lineNumber);
        }
        result.insert(result.begin() + op.lineNumber, op.content);
    }

    // Process Write operations - preserve sparse line numbers
    if (!writeOps.empty()) {
        size_t maxLine = writeOps.rbegin()->first;
        if (maxLine >= result.size()) {
            result.resize(maxLine + 1);
        }
        for (const auto& [lineNum, content] : writeOps) {
            result[lineNum] = content;
        }
    }

    // Process Append operations last
    for (const auto& content : appendOps) {
        result.push_back(content);
    }

    return result;
}

FileOperationHandle WriteBatch::commit() {
    return commit(WriteOptions{});
}

FileOperationHandle WriteBatch::commit(const WriteOptions& opts) {
    if (_operations.empty()) {
        // No operations to commit - return immediate success
        return FileOperationHandle::immediate(FileOpStatus::Complete);
    }
    
    auto backend = _vfs->findBackend(_path);
    auto vfsLock = _vfs->lockForPath(_path);

    return _vfs->submit(_path, [this, backend, vfsLock, ops = _operations, opts](FileOperationHandle::OpState& s, const std::string& p, const ExecContext&) mutable {
        // Prefer backend-provided write scope; fall back to VFS advisory lock with policy/timeout
        std::unique_ptr<void, void(*)(void*)> scopeToken(nullptr, [](void*){});
        IFileSystemBackend::AcquireWriteScopeResult scopeRes;
        if (backend) {
            IFileSystemBackend::AcquireScopeOptions scopeOpts;
            scopeOpts.nonBlocking = false;
            auto pol = _vfs ? _vfs->_cfg.advisoryFallback : VirtualFileSystem::Config::AdvisoryFallbackPolicy::FallbackWithTimeout;
            if (pol == VirtualFileSystem::Config::AdvisoryFallbackPolicy::FallbackWithTimeout || pol == VirtualFileSystem::Config::AdvisoryFallbackPolicy::None) {
                scopeOpts.timeout = _vfs ? std::optional<std::chrono::milliseconds>(_vfs->_cfg.advisoryAcquireTimeout) : std::nullopt;
            }
            scopeRes = backend->acquireWriteScope(p, scopeOpts);
            if (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::Acquired) {
                scopeToken = std::move(scopeRes.token);
            }
        }
        std::unique_lock<std::timed_mutex> pathLock;
        auto fallbackPolicy = _vfs ? _vfs->_cfg.advisoryFallback : VirtualFileSystem::Config::AdvisoryFallbackPolicy::FallbackWithTimeout;
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
                    if (fallbackPolicy == VirtualFileSystem::Config::AdvisoryFallbackPolicy::None) {
                        FileError code;
                        if (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::TimedOut) code = FileError::Timeout;
                        else if (scopeRes.status == IFileSystemBackend::AcquireWriteScopeResult::Status::Busy) code = FileError::Conflict;
                        else code = FileError::IOError;
                        s.setError(code, scopeRes.message.empty() ? std::string("Backend write scope unavailable") : scopeRes.message, p, scopeRes.errorCode);
                        s.complete(FileOpStatus::Failed);
                        return;
                    }
                }
                // Use bounded advisory fallback (including NotSupported)
                if (!vfsLock->try_lock_for(_vfs->_cfg.advisoryAcquireTimeout)) {
                    auto key = backend ? backend->normalizeKey(p) : _vfs->normalizePath(p);
                    auto ms = _vfs->_cfg.advisoryAcquireTimeout.count();
                    s.setError(FileError::Timeout, std::string("Advisory lock acquisition timed out after ") + std::to_string(ms) + " ms (key=" + key + ")", p);
                    s.complete(FileOpStatus::Failed);
                    return;
                }
                pathLock = std::unique_lock<std::timed_mutex>(*vfsLock, std::adopt_lock);
            }
        } else if (!scopeToken && !vfsLock) {
            // No serialization available; proceed best-effort
        } else {
            // scopeToken acquired; no advisory lock
        }
        
        std::error_code ec;
        
        // Determine line-ending style and original final-newline presence
        std::string data;
        bool originalFinalNewline = false;
        bool originalExists = false;
        std::string eol;
#if defined(_WIN32)
        const std::string platformDefaultEol = "\r\n";
#else
        const std::string platformDefaultEol = "\n";
#endif
        {
#if defined(_WIN32)
            auto osPathR = vfs_toWinLongPath(p);
#else
            const auto& osPathR = p;
#endif
            std::ifstream inBin(osPathR, std::ios::in | std::ios::binary);
            if (inBin) {
                originalExists = true;
                std::ostringstream ss; ss << inBin.rdbuf();
                data = ss.str();
                if (!data.empty() && data.back() == '\n') {
                    originalFinalNewline = true;
                }
                size_t crlf = 0, lf = 0;
                for (size_t i = 0; i < data.size(); ++i) {
                    if (data[i] == '\n') {
                        if (i > 0 && data[i-1] == '\r') ++crlf; else ++lf;
                    }
                }
                if (crlf > lf) eol = "\r\n"; else if (lf > crlf) eol = "\n"; else eol = platformDefaultEol;
            } else {
                eol = platformDefaultEol;
            }
        }
        if (eol.empty()) eol = platformDefaultEol;
        
        // Parse original lines without EOLs
        std::vector<std::string> originalLines;
        if (!data.empty()) {
            std::string cur;
            for (size_t i = 0; i < data.size(); ++i) {
                char c = data[i];
                if (c == '\n') {
                    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                    originalLines.push_back(std::move(cur));
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            if (!originalFinalNewline) {
                originalLines.push_back(std::move(cur));
            }
        }
        
        // Apply all operations to get final content
        std::vector<std::string> finalLines = applyOperations(originalLines);
        
        // Generate secure temporary filename in the same directory as destination
        auto targetPath = std::filesystem::path(p);
        auto dir = targetPath.parent_path();
        auto base = targetPath.filename().string();

#if defined(__unix__) || defined(__APPLE__)
        // Use mkstemp for secure temp file creation (avoid mutating std::string buffer)
        std::string tmpl = (dir / (base + ".XXXXXX")).string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = ::mkstemp(buf.data());
        std::filesystem::path tempPath;
        if (fd < 0) {
            // Fallback to random if mkstemp fails
            tempPath = dir / (base + ".tmp" + std::to_string(std::random_device{}()));
        } else {
            ::close(fd);  // We'll reopen with fstream
            tempPath = std::filesystem::path(buf.data());
        }
#else
        // Windows: use random device
        auto tempPath = dir / (base + ".tmp" + std::to_string(std::random_device{}()));
#endif
        
        // Ensure parent directory exists if configured (effective option)
        const bool createParents = opts.createParentDirs.value_or(_vfs ? _vfs->_cfg.defaultCreateParentDirs : false);
        if (createParents) {
            std::error_code dirEc;
            if (!dir.empty()) {
                std::filesystem::create_directories(dir, dirEc);
                if (dirEc) {
                    s.setError(FileError::IOError, "Failed to create parent directories", dir.string(), dirEc);
                    s.complete(FileOpStatus::Failed);
                    return;
                }
            }
        }
        
        // Optional cross-process serialization via sibling lock file (POSIX)
#if defined(__unix__) || defined(__APPLE__)
        struct LockFileToken { int fd = -1; ~LockFileToken(){ if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } } };
        std::unique_ptr<LockFileToken> lockToken;
        const bool useLockFile = opts.useLockFile.value_or(_vfs ? _vfs->_cfg.defaultUseLockFile : false);
        if (useLockFile) {
            auto timeout = opts.lockTimeout.value_or(_vfs ? _vfs->_cfg.lockAcquireTimeout : std::chrono::milliseconds(5000));
            auto suffix = opts.lockSuffix.value_or(_vfs ? _vfs->_cfg.lockSuffix : std::string(".lock"));
            std::error_code lockEc;
            bool timedOut = false;
            // Ensure parent directory exists for the lock file
            (void)createParents; // already handled above
            auto lockPath = (dir / (base + suffix)).string();
            int fd = ::open(lockPath.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
            if (fd < 0) {
                lockEc = std::error_code(errno, std::generic_category());
            } else {
                auto until = std::chrono::steady_clock::now() + timeout;
                for (;;) {
                    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                        lockToken = std::make_unique<LockFileToken>();
                        lockToken->fd = fd;
                        break;
                    }
                    int e = errno;
                    if (e != EWOULDBLOCK && e != EAGAIN) {
                        lockEc = std::error_code(e, std::generic_category());
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= until) {
                        timedOut = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (!lockToken && fd >= 0) { ::close(fd); }
            }
            if (!lockToken) {
                if (timedOut) {
                    s.setError(FileError::Timeout, "Lock acquisition timed out", p);
                } else if (lockEc) {
                    s.setError(FileError::IOError, "Failed to acquire lock file", p, lockEc);
                } else {
                    s.setError(FileError::IOError, "Failed to acquire lock file", p);
                }
                s.complete(FileOpStatus::Failed);
                return;
            }
        }
#endif

        // Decide final newline presence
        const bool finalNewline = opts.ensureFinalNewline.has_value()
            ? opts.ensureFinalNewline.value()
            : (originalExists ? originalFinalNewline : true);
        
        // Write to temp file in binary with consistent EOL
        {
            std::ofstream out(tempPath, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!out) {
                s.setError(FileError::IOError, "Failed to create temp file", tempPath.string());
                s.complete(FileOpStatus::Failed);
                return;
            }
            for (size_t i = 0; i < finalLines.size(); ++i) {
                const auto& ln = finalLines[i];
                out.write(ln.data(), static_cast<std::streamsize>(ln.size()));
                if (i < finalLines.size() - 1 || (i == finalLines.size() - 1 && finalNewline)) {
                    out.write(eol.data(), static_cast<std::streamsize>(eol.size()));
                }
            }
            if (finalLines.empty() && finalNewline) {
                // Preserve policy even for empty content
                out.write(eol.data(), static_cast<std::streamsize>(eol.size()));
            }
            out.flush();
            if (!out.good()) {
                std::filesystem::remove(tempPath, ec);
                s.setError(FileError::IOError, "Failed to write temp file", tempPath.string());
                s.complete(FileOpStatus::Failed);
                return;
            }
        }
        
        // Atomic rename
        #if defined(_WIN32)
            // Use Windows-specific atomic rename with retry logic
            auto wsrc = tempPath.wstring();
            auto wdst = targetPath.wstring();
            
            const int maxRetries = 50;
            const int retryDelayMs = 10;
            
            bool success = false;
            for (int i = 0; i < maxRetries; ++i) {
                if (MoveFileExW(wsrc.c_str(), wdst.c_str(), 
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
                    success = true;
                    break;
                }
                
                DWORD error = GetLastError();
                if (error == ERROR_SHARING_VIOLATION || 
                    error == ERROR_ACCESS_DENIED || 
                    error == ERROR_LOCK_VIOLATION) {
                    Sleep(retryDelayMs);
                    continue;
                }
                break;
            }
            
            if (!success) {
                std::filesystem::remove(tempPath, ec);
                s.setError(FileError::IOError, "Failed to replace destination file", p);
                s.complete(FileOpStatus::Failed);
                return;
            }
        #else
            // Preserve destination permissions if it exists
            {
                struct stat st;
                if (::stat(targetPath.c_str(), &st) == 0) {
                    ::chmod(tempPath.c_str(), st.st_mode & 07777);
                }
            }
            std::filesystem::rename(tempPath, targetPath, ec);
            if (ec) {
                std::filesystem::remove(tempPath, ec);
                s.setError(FileError::IOError, "Failed to rename temp file", p, ec);
                s.complete(FileOpStatus::Failed);
                return;
            }
            // If durability requested, fsync the parent directory to persist the rename
            if (opts.fsync) {
                std::error_code dirSyncEc;
                // Reuse same logic as POSIX helper locally
                auto parent = targetPath.parent_path();
                if (!parent.empty()) {
                    int dfd = ::open(parent.c_str(), O_RDONLY);
                    if (dfd >= 0) {
                        if (::fsync(dfd) != 0) {
                            dirSyncEc = std::error_code(errno, std::generic_category());
                        }
                        ::close(dfd);
                    } else {
                        dirSyncEc = std::error_code(errno, std::generic_category());
                    }
                }
                if (dirSyncEc) {
                    s.setError(FileError::IOError, "Directory fsync failed after rename", parent.string(), dirSyncEc);
                    s.complete(FileOpStatus::Failed);
                    return;
                }
            }
        #endif
        
        // Calculate total bytes written
        size_t totalBytes = 0;
        if (finalLines.empty()) {
            if (finalNewline) totalBytes = eol.size();
        } else {
            for (size_t i = 0; i < finalLines.size(); ++i) {
                totalBytes += finalLines[i].size();
                if (i < finalLines.size() - 1 || (i == finalLines.size() - 1 && finalNewline)) {
                    totalBytes += eol.size();
                }
            }
        }
        
        s.wrote = totalBytes;
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle WriteBatch::preview() const {
    return _vfs->submit(_path, [this, ops = _operations](FileOperationHandle::OpState& s, const std::string& p, const ExecContext&) {
        // Read the original file
        std::vector<std::string> originalLines;
        {
            std::ifstream in(p, std::ios::in);
            if (in) {
                std::string line;
                while (std::getline(in, line)) {
                    originalLines.push_back(line);
                }
            }
        }
        
        // Apply all operations to get final content
        std::vector<std::string> finalLines = applyOperations(originalLines);
        
        // Build result string
        std::ostringstream oss;
        for (size_t i = 0; i < finalLines.size(); ++i) {
            oss << finalLines[i];
            if (i < finalLines.size() - 1) {
                oss << "\n";
            }
        }
        
        s.text = oss.str();
        s.complete(FileOpStatus::Complete);
    });
}

} // namespace EntropyEngine::Core::IO