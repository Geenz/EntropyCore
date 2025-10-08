#include "LocalFileSystemBackend.h"
#include "VirtualFileSystem.h"
#include "FileStream.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cstring>
#include <random>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/file.h>  // flock()
#include <fcntl.h>     // open(), fcntl()
#include <unistd.h>    // close(), access(), fsync()
#include <sys/stat.h>  // stat(), chmod()
#endif

namespace EntropyEngine::Core::IO {

// Helper function for simple glob pattern matching
// Supports: * (any sequence), ? (single char)
namespace {
    bool matchGlob(const std::string& str, const std::string& pattern) {
        size_t s = 0, p = 0;
        size_t starIdx = std::string::npos, matchIdx = 0;

        while (s < str.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == str[s])) {
                // Match single character or exact match
                ++s;
                ++p;
            } else if (p < pattern.size() && pattern[p] == '*') {
                // Star matches zero or more characters
                starIdx = p;
                matchIdx = s;
                ++p;
            } else if (starIdx != std::string::npos) {
                // Backtrack to last star
                p = starIdx + 1;
                ++matchIdx;
                s = matchIdx;
            } else {
                // No match
                return false;
            }
        }

        // Skip remaining stars in pattern
        while (p < pattern.size() && pattern[p] == '*') {
            ++p;
        }

        return p == pattern.size();
    }

    // Map errno to FileError with platform-specific handling
    FileError mapErrnoToFileError(int err) {
        switch (err) {
            case ENOSPC:
#if defined(__unix__) || defined(__APPLE__)
            case EDQUOT:  // Disk quota exceeded (POSIX)
#endif
                return FileError::DiskFull;
            case EACCES:
            case EPERM:
                return FileError::AccessDenied;
            case ENOENT:
                return FileError::FileNotFound;
            case EINVAL:
            case ENAMETOOLONG:
#if defined(__unix__) || defined(__APPLE__)
            case EISDIR:
#endif
                return FileError::InvalidPath;
#if defined(__unix__) || defined(__APPLE__)
            case ENETUNREACH:
            case ENETDOWN:
            case ETIMEDOUT:
                return FileError::NetworkError;
#endif
            default:
                return FileError::IOError;
        }
    }

    // Check if path points to a special file (FIFO, device, socket)
    bool isSpecialFile(const std::filesystem::path& p) {
        std::error_code ec;
        auto status = std::filesystem::status(p, ec);
        if (ec) return false;

        return std::filesystem::is_block_file(status) ||
               std::filesystem::is_character_file(status) ||
               std::filesystem::is_fifo(status) ||
               std::filesystem::is_socket(status);
    }

    // Create secure temporary file path
    std::filesystem::path createSecureTempPath(const std::filesystem::path& dir,
                                               const std::string& base) {
#if defined(__unix__) || defined(__APPLE__)
        // Use mkstemp for secure temp file creation (avoid mutating std::string buffer)
        std::string tmpl = (dir / (base + ".XXXXXX")).string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = ::mkstemp(buf.data());
        if (fd < 0) {
            // Fallback to random if mkstemp fails
            return dir / (base + ".tmp" + std::to_string(std::random_device{}()));
        }
        ::close(fd);  // We'll reopen with fstream
        return std::filesystem::path(buf.data());
#else
        // Windows: use random device
        return dir / (base + ".tmp" + std::to_string(std::random_device{}()));
#endif
    }

    // Check current process permissions (platform-specific)
    bool checkCurrentProcessPermissions(const std::filesystem::path& p,
                                       bool& canRead, bool& canWrite, bool& canExec) {
#if defined(__unix__) || defined(__APPLE__)
        // Use access() for current process permissions
        canRead = (::access(p.c_str(), R_OK) == 0);
        canWrite = (::access(p.c_str(), W_OK) == 0);
        canExec = (::access(p.c_str(), X_OK) == 0);
        return true;
#else
        // Windows: use std::filesystem permissions (checks if anyone can access)
        std::error_code ec;
        auto st = std::filesystem::status(p, ec);
        if (ec) {
            canRead = canWrite = canExec = false;
            return false;
        }
        auto perms = st.permissions();
        auto has = [&](std::filesystem::perms perm) {
            return (perms & perm) != std::filesystem::perms::none;
        };
        canRead = has(std::filesystem::perms::owner_read) ||
                 has(std::filesystem::perms::group_read) ||
                 has(std::filesystem::perms::others_read);
        canWrite = has(std::filesystem::perms::owner_write) ||
                  has(std::filesystem::perms::group_write) ||
                  has(std::filesystem::perms::others_write);
        canExec = has(std::filesystem::perms::owner_exec) ||
                 has(std::filesystem::perms::group_exec) ||
                 has(std::filesystem::perms::others_exec);
        return true;
#endif
    }
}

// POSIX lock-file helpers and directory fsync for durability
namespace {
#if defined(__unix__) || defined(__APPLE__)
    struct PosixLockFile {
        int fd = -1;
        std::filesystem::path path;
        ~PosixLockFile() {
            if (fd >= 0) {
                // Best-effort unlock then close
                ::flock(fd, LOCK_UN);
                ::close(fd);
            }
        }
    };

    static std::unique_ptr<PosixLockFile> acquireSiblingLockPOSIX(
        const std::filesystem::path& dst,
        std::chrono::milliseconds timeout,
        const std::string& suffix,
        std::error_code& ecOut,
        bool& timedOut)
    {
        using clock = std::chrono::steady_clock;
        timedOut = false;
        ecOut.clear();
        auto lockPath = dst.string() + suffix;
        int fd = ::open(lockPath.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
        if (fd < 0) { ecOut = std::error_code(errno, std::generic_category()); return nullptr; }
        auto token = std::make_unique<PosixLockFile>();
        token->fd = fd;
        token->path = lockPath;

        auto until = clock::now() + timeout;
        for (;;) {
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                return token;
            }
            int e = errno;
            if (e != EWOULDBLOCK && e != EAGAIN) {
                ecOut = std::error_code(e, std::generic_category());
                return nullptr;
            }
            if (clock::now() >= until) {
                timedOut = true;
                return nullptr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    static bool fsyncParentDirectoryPOSIX(const std::filesystem::path& p, std::error_code& ecOut) {
        ecOut.clear();
        auto parent = p.parent_path();
        if (parent.empty()) return true;
        int dfd = ::open(parent.c_str(), O_RDONLY);
        if (dfd < 0) { ecOut = std::error_code(errno, std::generic_category()); return false; }
        bool ok = (::fsync(dfd) == 0);
        int e = ok ? 0 : errno;
        ::close(dfd);
        if (!ok) ecOut = std::error_code(e, std::generic_category());
        return ok;
    }
#endif
}

// Concrete FileStream implementation for local files
class LocalFileStream : public FileStream {
private:
    mutable std::fstream _stream;
    std::string _path;
    StreamOptions::Mode _mode;
    mutable bool _failFlag = false;
    
public:
    LocalFileStream(const std::string& path, StreamOptions::Mode mode) 
        : _path(path), _mode(mode) {
        std::ios_base::openmode flags = std::ios::binary;
        
        switch (mode) {
            case StreamOptions::Read:
                flags |= std::ios::in;
                break;
            case StreamOptions::Write:
                flags |= std::ios::out | std::ios::trunc;
                break;
            case StreamOptions::ReadWrite:
                flags |= std::ios::in | std::ios::out;
                break;
        }
        
        _stream.open(path, flags);
        if (!_stream.is_open()) {
            _failFlag = true;
        }
    }
    
    ~LocalFileStream() override {
        if (_stream.is_open()) {
            _stream.close();
        }
    }
    
    IoResult read(std::span<std::byte> buffer) override {
        IoResult result;
        
        if (!good() || buffer.empty()) {
            result.error = FileError::IOError;
            return result;
        }
        
        _stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        result.bytesTransferred = static_cast<size_t>(_stream.gcount());
        result.complete = (_stream.gcount() == static_cast<std::streamsize>(buffer.size())) || _stream.eof();
        
        if (_stream.bad()) {
            result.error = FileError::IOError;
        }
        
        return result;
    }
    
    IoResult write(std::span<const std::byte> data) override {
        IoResult result;
        
        if (!good() || data.empty()) {
            result.error = FileError::IOError;
            return result;
        }
        
        auto posBefore = _stream.tellp();
        _stream.write(reinterpret_cast<const char*>(data.data()), data.size());
        auto posAfter = _stream.tellp();
        
        if (_stream.good()) {
            result.bytesTransferred = static_cast<size_t>(posAfter - posBefore);
            result.complete = (result.bytesTransferred == data.size());
        } else {
            result.error = FileError::IOError;
        }
        
        return result;
    }
    
    bool seek(int64_t offset, std::ios_base::seekdir dir) override {
        if (_mode == StreamOptions::Read || _mode == StreamOptions::ReadWrite) {
            _stream.seekg(offset, dir);
        }
        if (_mode == StreamOptions::Write || _mode == StreamOptions::ReadWrite) {
            _stream.seekp(offset, dir);
        }
        return _stream.good();
    }
    
    int64_t tell() const override {
        if (_mode == StreamOptions::Read || _mode == StreamOptions::ReadWrite) {
            return static_cast<int64_t>(_stream.tellg());
        }
        if (_mode == StreamOptions::Write) {
            return static_cast<int64_t>(_stream.tellp());
        }
        return -1;
    }
    
    bool good() const override { return _stream.good() && !_failFlag; }
    bool eof() const override { return _stream.eof(); }
    bool fail() const override { return _stream.fail() || _failFlag; }
    void flush() override { _stream.flush(); }
    void close() override { _stream.close(); }
    std::string path() const override { return _path; }
};

// LocalFileSystemBackend implementation
LocalFileSystemBackend::LocalFileSystemBackend() {
}

FileOperationHandle LocalFileSystemBackend::submitWork(const std::string& path,
    std::function<void(FileOperationHandle::OpState&, const std::string&)> work) {
    
    if (!_vfs) {
        // Return a failed handle if no VFS is set - create inline
        auto state = std::make_shared<FileOperationHandle::OpState>();
        state->setError(FileError::Unknown, "No VirtualFileSystem set for backend");
        state->complete(FileOpStatus::Failed);
        return FileOperationHandle(state);
    }
    
    // Use VFS's submit method to execute work
    return _vfs->submit(path, work);
}

// Synchronous operations for FileHandle via submitSerialized
void LocalFileSystemBackend::doWriteFile(FileOperationHandle::OpState& s, const std::string& p, std::span<const std::byte> data, WriteOptions options) {
    // Check for special files (FIFO, device, socket) that shouldn't be written via file operations
    if (isSpecialFile(p)) {
        s.setError(FileError::InvalidPath, "Cannot perform file operations on special files (FIFO, device, socket)", p);
        s.complete(FileOpStatus::Failed);
        return;
    }

    std::error_code ec;
    const bool createParents = options.createParentDirs.value_or(_vfs ? _vfs->_cfg.defaultCreateParentDirs : false);
    if (createParents) {
        const auto parent = std::filesystem::path(p).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                s.setError(FileError::IOError, "Failed to create parent directories", p, ec);
                s.complete(FileOpStatus::Failed);
                return;
            }
        }
    }

    std::ios_base::openmode mode = std::ios::out | std::ios::binary;
    if (options.append) {
        mode |= std::ios::app;
    } else if (options.truncate || options.offset == 0) {
        mode |= std::ios::trunc;
    } else {
        mode |= std::ios::in;
    }

    std::fstream out(p, mode);
    if (!out && options.createIfMissing) {
        out.open(p, std::ios::out | std::ios::binary);
        out.close();
        out.open(p, mode);
    }

    if (!out) {
        const int saved_errno = errno;  // Capture errno immediately
        std::error_code pec;
        auto parent = std::filesystem::path(p).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent, pec)) {
            s.setError(FileError::InvalidPath, "Parent directory does not exist", p, pec);
        } else {
            std::error_code stEc;
            (void)std::filesystem::status(p, stEc);
            if (stEc) {
                s.setError(FileError::InvalidPath, "Invalid path or unsupported filename", p, stEc);
            } else {
                s.setError(FileError::AccessDenied, "Cannot open file for writing", p, std::error_code(saved_errno, std::generic_category()));
            }
        }
        s.complete(FileOpStatus::Failed);
        return;
    }

    if (options.offset > 0 && !options.append) {
        out.seekp(options.offset, std::ios::beg);
    }

    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    size_t wrote = data.size();

    if (options.ensureFinalNewline.value_or(false) && !options.append && (options.truncate || options.offset == 0)) {
    #if defined(_WIN32)
        const char* eol = "\r\n";
        const size_t eolLen = 2;
    #else
        const char* eol = "\n";
        const size_t eolLen = 1;
    #endif
        bool endsWithLF = (!data.empty() && reinterpret_cast<const char*>(data.data())[data.size() - 1] == '\n');
        if (!endsWithLF) {
            out.write(eol, static_cast<std::streamsize>(eolLen));
            wrote += eolLen;
        }
    }
    out.flush();
    if (!out.good()) {
        const int saved_errno = errno;  // Capture errno immediately
        auto code = mapErrnoToFileError(saved_errno);
        std::string msg = (code == FileError::DiskFull) ? "Disk full or quota exceeded" : "Write operation failed";
        s.setError(code, msg, p, std::error_code(saved_errno, std::generic_category()));
        s.complete(FileOpStatus::Failed);
        return;
    }

    // fsync to disk if requested (durability guarantee)
#if defined(__APPLE__)
    if (options.fsync) {
        out.close();
        int fd = ::open(p.c_str(), O_WRONLY);
        if (fd >= 0) {
            if (::fcntl(fd, F_FULLFSYNC) != 0) {
                const int sync_errno = errno;
                ::close(fd);
                s.setError(FileError::IOError, "F_FULLFSYNC failed", p, std::error_code(sync_errno, std::generic_category()));
                s.complete(FileOpStatus::Failed);
                return;
            }
            ::close(fd);
        }
    }
#elif defined(__linux__)
    if (options.fsync) {
        out.close();
        int fd = ::open(p.c_str(), O_WRONLY);
        if (fd >= 0) {
            if (::fdatasync(fd) != 0) {
                const int sync_errno = errno;
                ::close(fd);
                s.setError(FileError::IOError, "fdatasync failed", p, std::error_code(sync_errno, std::generic_category()));
                s.complete(FileOpStatus::Failed);
                return;
            }
            ::close(fd);
        }
    }
#elif defined(__unix__)
    if (options.fsync) {
        out.close();
        int fd = ::open(p.c_str(), O_WRONLY);
        if (fd >= 0) {
            if (::fsync(fd) != 0) {
                const int sync_errno = errno;
                ::close(fd);
                s.setError(FileError::IOError, "fsync failed", p, std::error_code(sync_errno, std::generic_category()));
                s.complete(FileOpStatus::Failed);
                return;
            }
            ::close(fd);
        }
    }
#endif

    s.wrote = wrote;
    s.complete(FileOpStatus::Complete);
}

void LocalFileSystemBackend::doDeleteFile(FileOperationHandle::OpState& s, const std::string& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);

    if (ec && std::filesystem::exists(p)) {
        s.setError(FileError::IOError, "Failed to delete file", p, ec);
        s.complete(FileOpStatus::Failed);
    } else {
        s.complete(FileOpStatus::Complete);
    }
}

void LocalFileSystemBackend::doCreateFile(FileOperationHandle::OpState& s, const std::string& p) {
    std::ofstream out(p, std::ios::out | std::ios::binary);
    if (!out) {
        const int saved_errno = errno;  // Capture errno immediately
        s.setError(FileError::AccessDenied, "Cannot create file", p, std::error_code(saved_errno, std::generic_category()));
        s.complete(FileOpStatus::Failed);
    } else {
        s.complete(FileOpStatus::Complete);
    }
}

void LocalFileSystemBackend::doWriteLine(FileOperationHandle::OpState& s, const std::string& p, size_t lineNumber, std::string_view line) {
    std::error_code ec;
    const bool createParents = _vfs ? _vfs->_cfg.defaultCreateParentDirs : false;
    if (createParents) {
        const auto parent = std::filesystem::path(p).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                s.setError(FileError::IOError, "Failed to create parent directories", p, ec);
                s.complete(FileOpStatus::Failed);
                return;
            }
        }
    }

    auto targetPath = std::filesystem::path(p);
    auto dir = targetPath.parent_path();
    auto base = targetPath.filename().string();

    // Optional cross-process serialization via sibling lock file (POSIX)
#if defined(__unix__) || defined(__APPLE__)
    std::unique_ptr<PosixLockFile> lockToken;
    if (_vfs && _vfs->_cfg.defaultUseLockFile) {
        std::error_code lockEc; bool timedOut = false;
        lockToken = acquireSiblingLockPOSIX(targetPath, _vfs->_cfg.lockAcquireTimeout, _vfs->_cfg.lockSuffix, lockEc, timedOut);
        if (!lockToken) {
            if (timedOut) {
                s.setError(FileError::Timeout, "Lock acquisition timed out", p);
            } else if (lockEc) {
                s.setError(mapErrnoToFileError(lockEc.value()), "Failed to acquire lock file", p, lockEc);
            } else {
                s.setError(FileError::IOError, "Failed to acquire lock file", p);
            }
            s.complete(FileOpStatus::Failed);
            return;
        }
    }
#endif

    auto tempPath = createSecureTempPath(dir, base);

    std::string data;
    bool originalFinalNewline = false;
    std::string eol;
#if defined(_WIN32)
    const std::string platformDefaultEol = "\r\n";
#else
    const std::string platformDefaultEol = "\n";
#endif
    {
        std::ifstream inBin(p, std::ios::in | std::ios::binary);
        if (inBin) {
            std::ostringstream ss; ss << inBin.rdbuf();
            data = ss.str();
            if (data.empty() || data.back() == '\n') {
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
            // File does not exist yet: adopt platform default EOL and default to final newline present
            eol = platformDefaultEol;
            originalFinalNewline = true; // backend default for new files: end with a newline
        }
    }
    if (eol.empty()) eol = platformDefaultEol;

    std::vector<std::string> linesVec;
    if (!data.empty()) {
        std::string cur;
        for (size_t i = 0; i < data.size(); ++i) {
            char c = data[i];
            if (c == '\n') {
                if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                linesVec.push_back(std::move(cur));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!originalFinalNewline) {
            linesVec.push_back(std::move(cur));
        }
    }

    if (lineNumber >= linesVec.size()) {
        linesVec.resize(lineNumber + 1);
    }
    linesVec[lineNumber] = std::string(line);

    std::ostringstream oss;
    for (size_t i = 0; i < linesVec.size(); ++i) {
        oss << linesVec[i];
        if (i < linesVec.size() - 1 || originalFinalNewline) oss << eol;
    }
    std::string finalData = oss.str();

    {
        std::ofstream tmp(tempPath, std::ios::out | std::ios::binary);
        if (!tmp) {
            s.setError(FileError::IOError, "Cannot create temp file for writeLine", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        tmp << finalData;
        if (!tmp.good()) {
            std::filesystem::remove(tempPath, ec);
            s.setError(FileError::IOError, "Failed to write temp file for writeLine", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
    }

#if defined(_WIN32)
    auto wsrc = tempPath.wstring();
    auto wdst = targetPath.wstring();
    const int maxRetries = 50;
    const int retryDelayMs = 10;
    bool replaced = false;
    for (int i = 0; i < maxRetries; ++i) {
        if (MoveFileExW(wsrc.c_str(), wdst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            replaced = true;
            break;
        }
        DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION) {
            if (i < maxRetries - 1) {
                Sleep(retryDelayMs);
                continue;
            }
        }
        break;
    }
    if (!replaced) {
        std::filesystem::remove(tempPath, ec);
        s.setError(FileError::IOError, "Failed to replace file with temp file for writeLine", p);
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
        s.setError(FileError::IOError, "Failed to replace file with temp file for writeLine", p, ec);
        s.complete(FileOpStatus::Failed);
        return;
    }
#endif

    s.wrote = finalData.size();
    s.complete(FileOpStatus::Complete);
}

FileOperationHandle LocalFileSystemBackend::readFile(const std::string& path, ReadOptions options) {
    return submitWork(path, [options](FileOperationHandle::OpState& s, const std::string& p) {
        // Check for special files (FIFO, device, socket)
        if (isSpecialFile(p)) {
            s.setError(FileError::InvalidPath, "Cannot perform file operations on special files (FIFO, device, socket)", p);
            s.complete(FileOpStatus::Failed);
            return;
        }

        std::ifstream in(p, std::ios::in | std::ios::binary);
        if (!in) {
            const int saved_errno = errno;  // Capture errno immediately
            std::error_code ec1;
            bool ex = std::filesystem::exists(p, ec1);
            if (ec1) {
                s.setError(FileError::InvalidPath, "Invalid path or unsupported filename", p, ec1);
            } else if (!ex) {
                s.setError(FileError::FileNotFound, "File not found", p);
            } else {
                s.setError(FileError::AccessDenied, "Cannot open file for reading", p, std::error_code(saved_errno, std::generic_category()));
            }
            s.complete(FileOpStatus::Failed);
            return;
        }
        
        if (options.offset > 0) {
            in.seekg(options.offset, std::ios::beg);
        }
        
        if (options.length.has_value()) {
            const size_t requested = options.length.value();
            s.bytes.resize(requested);
            in.read(reinterpret_cast<char*>(s.bytes.data()), requested);
            const auto got = static_cast<size_t>(in.gcount());
            s.bytes.resize(got);
            s.complete(got < requested ? FileOpStatus::Partial : FileOpStatus::Complete);
            return;
        } else {
            // Read entire file from offset
            in.seekg(0, std::ios::end);
            auto size = in.tellg();
            in.seekg(options.offset, std::ios::beg);
            size_t toRead = static_cast<size_t>(size - static_cast<std::streamoff>(options.offset));
            s.bytes.resize(toRead);
            in.read(reinterpret_cast<char*>(s.bytes.data()), toRead);
        }
        
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle LocalFileSystemBackend::writeFile(const std::string& path, std::span<const std::byte> data, WriteOptions options) {
    return submitWork(path, [this, data = std::vector<std::byte>(data.begin(), data.end()), options]
                            (FileOperationHandle::OpState& s, const std::string& p) mutable {

        // Check for special files (FIFO, device, socket)
        if (isSpecialFile(p)) {
            s.setError(FileError::InvalidPath, "Cannot perform file operations on special files (FIFO, device, socket)", p);
            s.complete(FileOpStatus::Failed);
            return;
        }

        std::error_code ec;
        // Determine effective parent creation policy
        const bool createParents = options.createParentDirs.value_or(_vfs ? _vfs->_cfg.defaultCreateParentDirs : false);
        if (createParents) {
            const auto parent = std::filesystem::path(p).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    s.setError(FileError::IOError, "Failed to create parent directories", p, ec);
                    s.complete(FileOpStatus::Failed);
                    return;
                }
            }
        }

        std::ios_base::openmode mode = std::ios::out | std::ios::binary;
        if (options.append) {
            mode |= std::ios::app;
        } else if (options.truncate || options.offset == 0) {
            mode |= std::ios::trunc;
        } else {
            mode |= std::ios::in;  // Need read mode to seek
        }

        std::fstream out(p, mode);
        if (!out && options.createIfMissing) {
            // Try to create the file
            out.open(p, std::ios::out | std::ios::binary);
            out.close();
            out.open(p, mode);
        }

        if (!out) {
            const int saved_errno = errno;  // Capture errno immediately
            std::error_code pec;
            auto parent = std::filesystem::path(p).parent_path();
            if (!parent.empty() && !std::filesystem::exists(parent, pec)) {
                s.setError(FileError::InvalidPath, "Parent directory does not exist", p, pec);
            } else {
                std::error_code stEc;
                (void)std::filesystem::status(p, stEc);
                if (stEc) {
                    s.setError(FileError::InvalidPath, "Invalid path or unsupported filename", p, stEc);
                } else {
                    s.setError(FileError::AccessDenied, "Cannot open file for writing", p, std::error_code(saved_errno, std::generic_category()));
                }
            }
            s.complete(FileOpStatus::Failed);
            return;
        }

        if (options.offset > 0 && !options.append) {
            out.seekp(options.offset, std::ios::beg);
        }

        // Perform write, honoring ensureFinalNewline for whole-file rewrites
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        size_t wrote = data.size();

        // If ensureFinalNewline is requested and this is a whole-file write (truncate or offset==0 and not append),
        // add a platform-default newline if the payload does not already end with '\n'.
        if (options.ensureFinalNewline.value_or(false) && !options.append && (options.truncate || options.offset == 0)) {
        #if defined(_WIN32)
            const char* eol = "\r\n";
            const size_t eolLen = 2;
        #else
            const char* eol = "\n";
            const size_t eolLen = 1;
        #endif
            bool endsWithLF = (!data.empty() && reinterpret_cast<const char*>(data.data())[data.size() - 1] == '\n');
            if (!endsWithLF) {
                out.write(eol, static_cast<std::streamsize>(eolLen));
                wrote += eolLen;
            }
        }
        out.flush();
        if (!out.good()) {
            const int saved_errno = errno;  // Capture errno immediately
            auto code = mapErrnoToFileError(saved_errno);
            std::string msg = (code == FileError::DiskFull) ? "Disk full or quota exceeded" : "Write operation failed";
            s.setError(code, msg, p, std::error_code(saved_errno, std::generic_category()));
            s.complete(FileOpStatus::Failed);
            return;
        }

        // fsync to disk if requested (durability guarantee)
#if defined(__APPLE__)
        if (options.fsync) {
            out.close();
            int fd = ::open(p.c_str(), O_WRONLY);
            if (fd >= 0) {
                if (::fcntl(fd, F_FULLFSYNC) != 0) {
                    const int sync_errno = errno;
                    ::close(fd);
                    s.setError(FileError::IOError, "F_FULLFSYNC failed", p, std::error_code(sync_errno, std::generic_category()));
                    s.complete(FileOpStatus::Failed);
                    return;
                }
                ::close(fd);
            }
        }
#elif defined(__linux__)
        if (options.fsync) {
            out.close();
            int fd = ::open(p.c_str(), O_WRONLY);
            if (fd >= 0) {
                if (::fdatasync(fd) != 0) {
                    const int sync_errno = errno;
                    ::close(fd);
                    s.setError(FileError::IOError, "fdatasync failed", p, std::error_code(sync_errno, std::generic_category()));
                    s.complete(FileOpStatus::Failed);
                    return;
                }
                ::close(fd);
            }
        }
#elif defined(__unix__)
        if (options.fsync) {
            out.close();
            int fd = ::open(p.c_str(), O_WRONLY);
            if (fd >= 0) {
                if (::fsync(fd) != 0) {
                    const int sync_errno = errno;
                    ::close(fd);
                    s.setError(FileError::IOError, "fsync failed", p, std::error_code(sync_errno, std::generic_category()));
                    s.complete(FileOpStatus::Failed);
                    return;
                }
                ::close(fd);
            }
        }
#endif

        s.wrote = wrote;
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle LocalFileSystemBackend::deleteFile(const std::string& path) {
    return submitWork(path, [](FileOperationHandle::OpState& s, const std::string& p) {
        
        std::error_code ec;
        std::filesystem::remove(p, ec);
        
        if (ec && std::filesystem::exists(p)) {
            s.setError(FileError::IOError, "Failed to delete file", p, ec);
            s.complete(FileOpStatus::Failed);
        } else {
            s.complete(FileOpStatus::Complete);
        }
    });
}

FileOperationHandle LocalFileSystemBackend::createFile(const std::string& path) {
    return submitWork(path, [this](FileOperationHandle::OpState& s, const std::string& p) {
        
        std::error_code ec;
        const bool createParents = _vfs ? _vfs->_cfg.defaultCreateParentDirs : false;
        if (createParents) {
            const auto parent = std::filesystem::path(p).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    s.setError(FileError::IOError, "Failed to create parent directories", p, ec);
                    s.complete(FileOpStatus::Failed);
                    return;
                }
            }
        }
        
        std::ofstream out(p, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) {
            std::error_code stEc;
            (void)std::filesystem::status(p, stEc);
            if (stEc) {
                s.setError(FileError::InvalidPath, "Invalid path or unsupported filename", p, stEc);
            } else {
                s.setError(FileError::AccessDenied, "Cannot create file", p, std::error_code(errno, std::generic_category()));
            }
            s.complete(FileOpStatus::Failed);
        } else {
            s.complete(FileOpStatus::Complete);
        }
    });
}

FileOperationHandle LocalFileSystemBackend::getMetadata(const std::string& path) {
    return submitWork(path, [](FileOperationHandle::OpState& s, const std::string& p) {
        std::error_code ec;
        auto status = std::filesystem::status(p, ec);

        FileMetadata meta;
        meta.path = p;

        if (ec) {
            // File doesn't exist or can't be accessed
            meta.exists = false;
            s.metadata = meta;
            s.complete(FileOpStatus::Complete);
            return;
        }

        meta.exists = std::filesystem::exists(status);

        // Determine whether the path itself is a symlink (do not follow)
        std::error_code ssec;
        auto ss = std::filesystem::symlink_status(p, ssec);
        meta.isSymlink = !ssec && std::filesystem::is_symlink(ss);

        if (meta.exists) {
            meta.isDirectory = std::filesystem::is_directory(status);
            meta.isRegularFile = std::filesystem::is_regular_file(status);

            // Get file size for regular files (follow target)
            if (meta.isRegularFile) {
                meta.size = std::filesystem::file_size(p, ec);
                if (ec) meta.size = 0;
            }

            // Get permissions - use platform-specific check for current process
            checkCurrentProcessPermissions(p, meta.readable, meta.writable, meta.executable);

            // Get last modified time
            auto lwt = std::filesystem::last_write_time(p, ec);
            if (!ec) {
                // Convert file_time_type to system_clock::time_point
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    lwt - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
                );
                meta.lastModified = sctp;
            }
        }

        s.metadata = meta;
        s.complete(FileOpStatus::Complete);
    });
}

bool LocalFileSystemBackend::exists(const std::string& path) {
    return std::filesystem::exists(path);
}

FileOperationHandle LocalFileSystemBackend::getMetadataBatch(const BatchMetadataOptions& options) {
    return submitWork("batch_metadata", [options](FileOperationHandle::OpState& s, const std::string&) {
        std::vector<FileMetadata> results;
        results.reserve(options.paths.size());

#if defined(_WIN32)
        // Windows optimization: use FindFirstFileEx with Basic info level
        // This is significantly faster than individual stat calls
        for (const auto& path : options.paths) {
            FileMetadata meta;
            meta.path = path;

            WIN32_FIND_DATAW findData;
            HANDLE hFind = FindFirstFileExW(
                std::filesystem::path(path).wstring().c_str(),
                FindExInfoBasic,  // Don't retrieve short names - faster!
                &findData,
                FindExSearchNameMatch,
                nullptr,
                FIND_FIRST_EX_LARGE_FETCH  // Optimize for batch queries
            );

            if (hFind == INVALID_HANDLE_VALUE) {
                meta.exists = false;
                results.push_back(std::move(meta));
                continue;
            }

            meta.exists = true;
            meta.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            meta.isRegularFile = !meta.isDirectory &&
                                 (findData.dwFileAttributes & FILE_ATTRIBUTE_NORMAL) != 0;
            meta.isSymlink = (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

            // Calculate file size
            LARGE_INTEGER fileSize;
            fileSize.LowPart = findData.nFileSizeLow;
            fileSize.HighPart = findData.nFileSizeHigh;
            meta.size = static_cast<uintmax_t>(fileSize.QuadPart);

            // Permissions (simplified on Windows)
            meta.readable = true;  // If we can find it, we can read it
            meta.writable = (findData.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0;
            meta.executable = false;  // Would need more complex check

            // Convert FILETIME to system_clock::time_point
            FILETIME ft = findData.ftLastWriteTime;
            ULARGE_INTEGER ull;
            ull.LowPart = ft.dwLowDateTime;
            ull.HighPart = ft.dwHighDateTime;

            // Windows FILETIME is 100-nanosecond intervals since 1601-01-01
            // Convert to Unix epoch (1970-01-01)
            const uint64_t EPOCH_DIFFERENCE = 116444736000000000ULL;
            uint64_t unixTime = (ull.QuadPart - EPOCH_DIFFERENCE) / 10000000ULL;
            meta.lastModified = std::chrono::system_clock::from_time_t(static_cast<time_t>(unixTime));

            FindClose(hFind);
            results.push_back(std::move(meta));
        }
#else
        // Linux/macOS: use standard filesystem API
        // TODO: Could optimize with statx() batch calls on Linux
        std::error_code ec;
        for (const auto& path : options.paths) {
            FileMetadata meta;
            meta.path = path;

            auto status = std::filesystem::status(path, ec);
            if (ec) {
                meta.exists = false;
                results.push_back(std::move(meta));
                continue;
            }

            meta.exists = std::filesystem::exists(status);
            if (meta.exists) {
                meta.isDirectory = std::filesystem::is_directory(status);
                meta.isRegularFile = std::filesystem::is_regular_file(status);
                meta.isSymlink = std::filesystem::is_symlink(status);

                if (meta.isRegularFile) {
                    meta.size = std::filesystem::file_size(path, ec);
                    if (ec) meta.size = 0;
                }

                // Get permissions
                auto perms = status.permissions();
                auto has = [&](std::filesystem::perms perm) {
                    return (perms & perm) != std::filesystem::perms::none;
                };
                meta.readable = has(std::filesystem::perms::owner_read) ||
                               has(std::filesystem::perms::group_read) ||
                               has(std::filesystem::perms::others_read);
                meta.writable = has(std::filesystem::perms::owner_write) ||
                               has(std::filesystem::perms::group_write) ||
                               has(std::filesystem::perms::others_write);
                meta.executable = has(std::filesystem::perms::owner_exec) ||
                                 has(std::filesystem::perms::group_exec) ||
                                 has(std::filesystem::perms::others_exec);

                // Get last modified time
                auto lwt = std::filesystem::last_write_time(path, ec);
                if (!ec) {
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        lwt - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
                    );
                    meta.lastModified = sctp;
                }
            }

            results.push_back(std::move(meta));
        }
#endif

        s.metadataBatch = std::move(results);
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle LocalFileSystemBackend::createDirectory(const std::string& path) {
    return submitWork(path, [](FileOperationHandle::OpState& s, const std::string& p) {
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        
        if (ec) {
            s.setError(FileError::IOError, "Cannot create directory", p, ec);
            s.complete(FileOpStatus::Failed);
        } else {
            s.complete(FileOpStatus::Complete);
        }
    });
}

FileOperationHandle LocalFileSystemBackend::removeDirectory(const std::string& path) {
    return submitWork(path, [](FileOperationHandle::OpState& s, const std::string& p) {
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        
        if (ec) {
            s.setError(FileError::IOError, "Cannot remove directory", p, ec);
            s.complete(FileOpStatus::Failed);
        } else {
            s.complete(FileOpStatus::Complete);
        }
    });
}

FileOperationHandle LocalFileSystemBackend::listDirectory(const std::string& path, ListDirectoryOptions options) {
    return submitWork(path, [options](FileOperationHandle::OpState& s, const std::string& p) {
        try {
            std::vector<DirectoryEntry> entries;
            std::error_code ec;

            // Fail fast if directory does not exist
            if (!std::filesystem::exists(p, ec)) {
                s.setError(FileError::FileNotFound, "Directory not found", p, ec);
                s.complete(FileOpStatus::Failed);
                return;
            }

            // Helper to populate DirectoryEntry from filesystem::directory_entry
            auto populateEntry = [&](const std::filesystem::directory_entry& fsEntry, size_t depth) -> bool {
                // Check depth limit
                if (depth > options.maxDepth) {
                    return false;  // Don't recurse further
                }

                DirectoryEntry entry;
                entry.name = fsEntry.path().filename().string();
                entry.fullPath = fsEntry.path().string();

                // Get file status
                auto status = fsEntry.status(ec);
                if (ec) return true;  // Skip this entry but continue

                // Populate metadata
                FileMetadata& meta = entry.metadata;
                meta.path = entry.fullPath;
                meta.exists = true;
                meta.isDirectory = std::filesystem::is_directory(status);
                meta.isRegularFile = std::filesystem::is_regular_file(status);
                meta.isSymlink = std::filesystem::is_symlink(status);

                entry.isSymlink = meta.isSymlink;

                // Get symlink target
                if (meta.isSymlink) {
                    auto target = std::filesystem::read_symlink(fsEntry.path(), ec);
                    if (!ec) {
                        entry.symlinkTarget = target.string();
                    }
                }

                // Get file size for regular files
                if (meta.isRegularFile) {
                    meta.size = fsEntry.file_size(ec);
                    if (ec) meta.size = 0;
                }

                // Get permissions
                auto perms = status.permissions();
                auto has = [&](std::filesystem::perms perm) {
                    return (perms & perm) != std::filesystem::perms::none;
                };
                meta.readable = has(std::filesystem::perms::owner_read) ||
                               has(std::filesystem::perms::group_read) ||
                               has(std::filesystem::perms::others_read);
                meta.writable = has(std::filesystem::perms::owner_write) ||
                               has(std::filesystem::perms::group_write) ||
                               has(std::filesystem::perms::others_write);
                meta.executable = has(std::filesystem::perms::owner_exec) ||
                                 has(std::filesystem::perms::group_exec) ||
                                 has(std::filesystem::perms::others_exec);

                // Get last modified time
                auto lwt = fsEntry.last_write_time(ec);
                if (!ec) {
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        lwt - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
                    );
                    meta.lastModified = sctp;
                }

                // Filter hidden files if requested
                if (!options.includeHidden) {
                    // Check if file is hidden (platform-specific)
#if defined(_WIN32)
                    // On Windows, check FILE_ATTRIBUTE_HIDDEN
                    auto attrs = GetFileAttributesW(fsEntry.path().c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN)) {
                        return true;  // Skip hidden file
                    }
#else
                    // On Unix-like systems, files starting with '.' are hidden
                    if (!entry.name.empty() && entry.name[0] == '.') {
                        return true;  // Skip hidden file
                    }
#endif
                }

                // Apply glob pattern filter if specified
                if (options.globPattern.has_value()) {
                    // Simple glob matching: * matches any sequence, ? matches single char
                    const auto& pattern = options.globPattern.value();
                    if (!matchGlob(entry.name, pattern)) {
                        return true;  // Skip this entry
                    }
                }

                // Apply custom filter if specified
                if (options.filter && !options.filter(entry)) {
                    return true;  // Skip this entry
                }

                entries.push_back(std::move(entry));

                return true;
            };

            // Iterate directory
            if (options.recursive) {
                auto dirOptions = std::filesystem::directory_options::skip_permission_denied;
                if (options.followSymlinks) {
                    dirOptions |= std::filesystem::directory_options::follow_directory_symlink;
                }

                size_t currentDepth = 0;
                for (const auto& fsEntry : std::filesystem::recursive_directory_iterator(p, dirOptions, ec)) {
                    if (ec) break;

                    // Calculate depth
                    auto relativePath = std::filesystem::relative(fsEntry.path(), p, ec);
                    if (!ec) {
                        auto segments = static_cast<size_t>(std::distance(relativePath.begin(), relativePath.end()));
                        currentDepth = segments > 0 ? segments - 1 : 0;
                    }

                    populateEntry(fsEntry, currentDepth);
                }
            } else {
                for (const auto& fsEntry : std::filesystem::directory_iterator(p, ec)) {
                    if (ec) break;
                    populateEntry(fsEntry, 0);
                }
            }

            if (ec && entries.empty()) {
                s.setError(FileError::IOError, "Cannot iterate directory", p, ec);
                s.complete(FileOpStatus::Failed);
                return;
            }

            // Sort results if requested
            if (options.sortBy != ListDirectoryOptions::None) {
                switch (options.sortBy) {
                    case ListDirectoryOptions::ByName:
                        std::sort(entries.begin(), entries.end(), [](const DirectoryEntry& a, const DirectoryEntry& b) {
                            return a.name < b.name;
                        });
                        break;
                    case ListDirectoryOptions::BySize:
                        std::sort(entries.begin(), entries.end(), [](const DirectoryEntry& a, const DirectoryEntry& b) {
                            return a.metadata.size < b.metadata.size;
                        });
                        break;
                    case ListDirectoryOptions::ByModifiedTime:
                        std::sort(entries.begin(), entries.end(), [](const DirectoryEntry& a, const DirectoryEntry& b) {
                            if (!a.metadata.lastModified.has_value()) return false;
                            if (!b.metadata.lastModified.has_value()) return true;
                            return a.metadata.lastModified.value() < b.metadata.lastModified.value();
                        });
                        break;
                    default:
                        break;
                }
            }

            // Apply pagination after sorting to ensure top-N by requested order
            if (options.maxResults > 0 && entries.size() > options.maxResults) {
                entries.resize(options.maxResults);
            }

            s.directoryEntries = std::move(entries);
            s.complete(FileOpStatus::Complete);

        } catch (const std::filesystem::filesystem_error& e) {
            s.setError(FileError::IOError, e.what(), p);
            s.complete(FileOpStatus::Failed);
        }
    });
}

std::unique_ptr<FileStream> LocalFileSystemBackend::openStream(const std::string& path, StreamOptions options) {
    return std::make_unique<LocalFileStream>(path, options.mode);
}

FileOperationHandle LocalFileSystemBackend::readLine(const std::string& path, size_t lineNumber) {
    return submitWork(path, [lineNumber](FileOperationHandle::OpState& s, const std::string& p) {
        std::ifstream in(p, std::ios::in);
        if (!in) {
            s.setError(FileError::FileNotFound, "File not found or cannot be opened", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        
        std::string line;
        size_t currentLine = 0;
        while (std::getline(in, line)) {
            if (currentLine == lineNumber) {
                // Strip trailing \r if present (for CRLF line endings on Unix)
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                s.bytes.resize(line.size());
                std::memcpy(s.bytes.data(), line.data(), line.size());
                s.complete(FileOpStatus::Complete);
                return;
            }
            currentLine++;
        }
        
        s.complete(FileOpStatus::Partial);  // Line not found
    });
}

FileOperationHandle LocalFileSystemBackend::writeLine(const std::string& path, size_t lineNumber, std::string_view line) {
    return submitWork(path, [this, lineNumber, line = std::string(line)]
                            (FileOperationHandle::OpState& s, const std::string& p) mutable {
        
        std::error_code ec;
        
        // Ensure destination parent directory exists if configured
        const bool createParents = _vfs ? _vfs->_cfg.defaultCreateParentDirs : false;
        if (createParents) {
            const auto parent = std::filesystem::path(p).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    s.setError(FileError::IOError, "Failed to create parent directories", p, ec);
                    s.complete(FileOpStatus::Failed);
                    return;
                }
            }
        }
        
        // Generate temporary filename in the same directory as destination for atomic replace
        auto targetPath = std::filesystem::path(p);
        auto dir = targetPath.parent_path();
        auto base = targetPath.filename().string();
        auto tempPath = createSecureTempPath(dir, base);
        
        // Determine existing file content and line-ending style
        std::string data;
        bool originalFinalNewline = false;
        std::string eol;
#if defined(_WIN32)
        const std::string platformDefaultEol = "\r\n";
#else
        const std::string platformDefaultEol = "\n";
#endif
        {
            std::ifstream inBin(p, std::ios::in | std::ios::binary);
            if (inBin) {
                std::ostringstream ss; ss << inBin.rdbuf();
                data = ss.str();
                if (!data.empty()) {
                    if (data.back() == '\n') {
                        originalFinalNewline = true;
                    }
                }
                // Detect dominant EOL
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
        
        // Parse existing lines without EOLs
        std::vector<std::string> linesVec;
        if (!data.empty()) {
            std::string cur;
            for (size_t i = 0; i < data.size(); ++i) {
                char c = data[i];
                if (c == '\n') {
                    // If previous char was \r, drop it
                    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                    linesVec.push_back(std::move(cur));
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            if (!originalFinalNewline) {
                // Last line without trailing newline remains
                linesVec.push_back(std::move(cur));
            }
        }
        
        // Apply writeLine semantics
        if (lineNumber < linesVec.size()) {
            linesVec[lineNumber] = line;
        } else {
            // extend with blanks up to lineNumber, then add line
            while (linesVec.size() < lineNumber) {
                linesVec.emplace_back("");
            }
            if (linesVec.size() == lineNumber) {
                linesVec.emplace_back(line);
            }
        }
        
        // Write to temp file using chosen EOL and preserving original final-newline presence
        {
            std::ofstream out(tempPath, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!out) {
                s.setError(FileError::IOError, "Failed to create temp file", tempPath.string());
                s.complete(FileOpStatus::Failed);
                return;
            }
            // Decide final newline presence: if original had no content, default to true; otherwise preserve prior policy
            const bool finalNewline = data.empty() ? true : originalFinalNewline;
            for (size_t i = 0; i < linesVec.size(); ++i) {
                out.write(linesVec[i].data(), static_cast<std::streamsize>(linesVec[i].size()));
                // Add EOL if not last line, or if last line and policy requires final newline
                if (i < linesVec.size() - 1 || (i == linesVec.size() - 1 && finalNewline)) {
                    out.write(eol.data(), static_cast<std::streamsize>(eol.size()));
                }
            }
            if (linesVec.empty() && finalNewline) {
                // Edge case: empty file but policy requires final newline -> write one EOL
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
        
        // Atomic replace/rename
        #if defined(_WIN32)
            // Use Windows-specific atomic rename with retry logic (to avoid sharing violations)
            auto wsrc = tempPath.wstring();
            auto wdst = targetPath.wstring();
            
            const int maxRetries = 50;
            const int retryDelayMs = 10;
            bool success = false;
            for (int i = 0; i < maxRetries; ++i) {
                if (MoveFileExW(wsrc.c_str(), wdst.c_str(), 
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
                    success = true; break;
                }
                DWORD error = GetLastError();
                if (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION) {
                    Sleep(retryDelayMs); continue;
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
            std::filesystem::rename(tempPath, targetPath, ec);
            if (ec) {
                std::filesystem::remove(tempPath, ec);
                s.setError(FileError::IOError, "Failed to rename temp file", p, ec);
                s.complete(FileOpStatus::Failed);
                return;
            }
        #endif
        
        s.wrote = line.size();
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle LocalFileSystemBackend::copyFile(const std::string& src, const std::string& dst, const CopyOptions& options) {
    return submitWork(src, [this, src, dst, options](FileOperationHandle::OpState& s, const std::string&) {
        std::error_code ec;

        // Ensure destination parent directory exists if configured
        const bool createParents = _vfs ? _vfs->_cfg.defaultCreateParentDirs : false;
        if (createParents) {
            const auto parent = std::filesystem::path(dst).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    s.setError(FileError::IOError, "Failed to create destination parent directories", dst, ec);
                    s.complete(FileOpStatus::Failed);
                    return;
                }
            }
        }

        // Check if source exists
        if (!std::filesystem::exists(src, ec) || ec) {
            s.setError(FileError::FileNotFound, "Source file not found", src, ec);
            s.complete(FileOpStatus::Failed);
            return;
        }

        // Check if destination exists
        if (std::filesystem::exists(dst, ec) && !options.overwriteExisting) {
            s.setError(FileError::AccessDenied, "Destination already exists", dst);
            s.complete(FileOpStatus::Failed);
            return;
        }

        // Get source file size for progress reporting
        uintmax_t fileSize = std::filesystem::file_size(src, ec);
        if (ec) fileSize = 0;

        // Try copy-on-write first if requested (reflink on Linux/APFS)
        if (options.useReflink) {
#if defined(__linux__)
            // On Linux, try copy_file with copy_options::create_hard_links to test CoW
            // If filesystem supports it (Btrfs, XFS with reflink), this will be instant
            std::filesystem::copy_options copyOpts = std::filesystem::copy_options::overwrite_existing;
            if (std::filesystem::copy_file(src, dst, copyOpts, ec)) {
                s.wrote = fileSize;
                s.complete(FileOpStatus::Complete);
                return;
            }
            // Fall through to regular copy if reflink not supported
            ec.clear();
#endif
        }

        // Regular copy with progress callback
        if (options.progressCallback && fileSize > 0) {
            // Chunked copy with progress
            std::ifstream in(src, std::ios::binary);
            std::ofstream out(dst, std::ios::binary | std::ios::trunc);

            if (!in || !out) {
                s.setError(FileError::IOError, "Cannot open files for copying", src);
                s.complete(FileOpStatus::Failed);
                return;
            }

            const size_t chunkSize = 1024 * 1024;  // 1MB chunks
            std::vector<char> buffer(chunkSize);
            size_t totalCopied = 0;

            while (in && totalCopied < fileSize) {
                in.read(buffer.data(), chunkSize);
                std::streamsize bytesRead = in.gcount();

                if (bytesRead > 0) {
                    out.write(buffer.data(), bytesRead);
                    totalCopied += static_cast<size_t>(bytesRead);

                    // Call progress callback - if it returns false, cancel
                    if (!options.progressCallback(totalCopied, fileSize)) {
                        // Ensure streams are closed before cleanup on Windows
                        out.flush(); out.close(); in.close();
                        s.setError(FileError::Unknown, "Copy cancelled by user", src);
                        s.complete(FileOpStatus::Failed);
                        std::filesystem::remove(dst, ec);  // Clean up partial copy
                        return;
                    }
                }
            }

            if (!out.good()) {
                // Close streams before attempting cleanup
                out.flush(); out.close(); in.close();
                s.setError(FileError::IOError, "Write error during copy", dst);
                s.complete(FileOpStatus::Failed);
                std::filesystem::remove(dst, ec);
                return;
            }

            s.wrote = totalCopied;
        } else {
            // Fast copy without progress using std::filesystem
            std::filesystem::copy_options copyOpts = options.overwriteExisting ?
                std::filesystem::copy_options::overwrite_existing :
                std::filesystem::copy_options::none;

            if (!std::filesystem::copy_file(src, dst, copyOpts, ec)) {
                s.setError(FileError::IOError, "Copy failed", src, ec);
                s.complete(FileOpStatus::Failed);
                return;
            }

            s.wrote = fileSize;
        }

        // Preserve attributes if requested
        if (options.preserveAttributes) {
            std::filesystem::last_write_time(dst, std::filesystem::last_write_time(src, ec), ec);
            std::filesystem::permissions(dst, std::filesystem::status(src, ec).permissions(), ec);
        }

        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle LocalFileSystemBackend::moveFile(const std::string& src, const std::string& dst, bool overwriteExisting) {
    return submitWork(src, [this, src, dst, overwriteExisting](FileOperationHandle::OpState& s, const std::string&) {
        std::error_code ec;

        // Ensure destination parent directory exists if configured
        const bool createParents = _vfs ? _vfs->_cfg.defaultCreateParentDirs : false;
        if (createParents) {
            const auto parent = std::filesystem::path(dst).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    s.setError(FileError::IOError, "Failed to create destination parent directories", dst, ec);
                    s.complete(FileOpStatus::Failed);
                    return;
                }
            }
        }

        // Check if source exists
        if (!std::filesystem::exists(src, ec) || ec) {
            s.setError(FileError::FileNotFound, "Source file not found", src, ec);
            s.complete(FileOpStatus::Failed);
            return;
        }

        // Check if destination exists
        if (std::filesystem::exists(dst, ec) && !overwriteExisting) {
            s.setError(FileError::AccessDenied, "Destination already exists", dst);
            s.complete(FileOpStatus::Failed);
            return;
        }

        uintmax_t fileSize = std::filesystem::file_size(src, ec);
        if (ec) fileSize = 0;

        // Remove destination if overwriting
        if (overwriteExisting && std::filesystem::exists(dst, ec)) {
            std::filesystem::remove(dst, ec);
            ec.clear(); // Clear error if removal fails - rename/copy will handle it
        }

        // Try rename first (atomic if on same filesystem)
        std::filesystem::rename(src, dst, ec);

        if (!ec) {
            // Success - rename worked (same filesystem)
            s.wrote = fileSize;
            s.complete(FileOpStatus::Complete);
            return;
        }

        // Rename failed (likely cross-filesystem) - do copy + delete
        ec.clear();
        if (!std::filesystem::copy_file(src, dst,
            overwriteExisting ? std::filesystem::copy_options::overwrite_existing :
                              std::filesystem::copy_options::none, ec)) {
            s.setError(FileError::IOError, "Copy failed during move", src, ec);
            s.complete(FileOpStatus::Failed);
            return;
        }

        // Copy succeeded, now delete source
        std::filesystem::remove(src, ec);
        if (ec) {
            // Copy succeeded but delete failed - partial success
            s.setError(FileError::IOError, "Source deletion failed after copy", src, ec);
            s.complete(FileOpStatus::Partial);
            return;
        }

        s.wrote = fileSize;
        s.complete(FileOpStatus::Complete);
    });
}

BackendCapabilities LocalFileSystemBackend::getCapabilities() const {
    BackendCapabilities caps;
    caps.supportsStreaming = true;
    caps.supportsRandomAccess = true;
    caps.supportsDirectories = true;
    caps.supportsMetadata = true;
    caps.supportsAtomicWrites = true;  // via rename
    caps.supportsWatching = false;     // not implemented yet
    caps.isRemote = false;
    caps.maxFileSize = SIZE_MAX;
    return caps;
}

} // namespace EntropyEngine::Core::IO

// Backend-aware path normalization for LocalFileSystemBackend
std::string EntropyEngine::Core::IO::LocalFileSystemBackend::normalizeKey(const std::string& path) const {
    if (_vfs) {
        // Use VFS normalization (canonical + case-insensitive on Windows)
        return _vfs->normalizePath(path);
    }
    // Fallback similar to VFS normalizePath
    std::error_code ec;
    auto p = std::filesystem::path(path);
    auto canon = std::filesystem::weakly_canonical(p, ec);
    std::string s = (ec ? p.lexically_normal().string() : canon.string());
#if defined(_WIN32)
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
#endif
    return s;
}

// Cross-process file locking implementation (Unix/POSIX only)
//
// NOTE: flock() is currently DISABLED because it's incompatible with atomic file replacement.
// When we use temp file + rename for atomic writes (doWriteLine, WriteBatch), flock() locks
// the original inode, but rename() changes which inode the path points to, breaking the lock.
//
// We return NotSupported to fall back to VFS in-process advisory locks. For true cross-process
// locking with atomic replacement, we'd need to:
// 1. Lock a separate .lock file (adds complexity)
// 2. Use directory-based locking
// 3. Use a lock server/database
//
// For now, VFS provides in-process serialization which is sufficient for most use cases.
EntropyEngine::Core::IO::IFileSystemBackend::AcquireWriteScopeResult EntropyEngine::Core::IO::LocalFileSystemBackend::acquireWriteScope(const std::string& path, EntropyEngine::Core::IO::IFileSystemBackend::AcquireScopeOptions options) {
    (void)path;
    (void)options;

    AcquireWriteScopeResult result;
    result.status = AcquireWriteScopeResult::Status::NotSupported;
    result.message = "flock() disabled - incompatible with atomic file replacement (temp+rename). Using VFS advisory locks.";
    return result;
}
