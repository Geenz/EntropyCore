#include "FileHandle.h"
#include "VirtualFileSystem.h"
#include "IFileSystemBackend.h"
#include "LocalFileSystemBackend.h"
#include "FileStream.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cstring>
#include <random>
#if defined(_WIN32)
#include <windows.h>
#endif

using EntropyEngine::Core::Concurrency::ExecutionType;

namespace EntropyEngine::Core::IO {

// Helper
namespace {
    static bool read_text_file(const std::string& path, std::string& out) {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) return false;
        std::ostringstream ss; ss << in.rdbuf();
        out = ss.str();
        return true;
    }

#if defined(_WIN32)
    static bool win_replace_file(const std::filesystem::path& src, const std::filesystem::path& dst) {
        auto wsrc = src.wstring();
        auto wdst = std::filesystem::path(dst).wstring();
        
        // Retry logic for Windows file operations which can fail due to sharing violations
        const int maxRetries = 50;
        const int retryDelayMs = 10;
        
        for (int i = 0; i < maxRetries; ++i) {
            if (MoveFileExW(wsrc.c_str(), wdst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
                return true;  // Success
            }
            
            DWORD error = GetLastError();
            // Retry on sharing violations and access denied (file might be in use)
            if (error == ERROR_SHARING_VIOLATION || 
                error == ERROR_ACCESS_DENIED || 
                error == ERROR_LOCK_VIOLATION) {
                if (i < maxRetries - 1) {
                    Sleep(retryDelayMs);
                    continue;
                }
            }
            break;  // Other error or max retries reached
        }
        return false;
    }
#endif
}

FileHandle::FileHandle(VirtualFileSystem* vfs, std::string path)
    : _vfs(vfs) {
    _meta.path = std::move(path);
    std::filesystem::path pp(_meta.path);
    _meta.directory = pp.has_parent_path() ? pp.parent_path().string() : std::string();
    _meta.filename = pp.filename().string();
    _meta.extension = pp.has_extension() ? pp.extension().string() : std::string();
    std::error_code ec;
    auto st = std::filesystem::status(pp, ec);
    _meta.exists = !ec && std::filesystem::exists(st);
    if (_meta.exists) {
        if (std::filesystem::is_regular_file(st)) {
            _meta.size = std::filesystem::file_size(pp, ec);
            if (ec) _meta.size = 0;
        } else {
            _meta.size = 0;
        }
        auto perms = st.permissions();
        auto has = [&](std::filesystem::perms p){ return (perms & p) != std::filesystem::perms::none; };
        _meta.canRead = has(std::filesystem::perms::owner_read) || has(std::filesystem::perms::group_read) || has(std::filesystem::perms::others_read);
        _meta.canWrite = has(std::filesystem::perms::owner_write) || has(std::filesystem::perms::group_write) || has(std::filesystem::perms::others_write);
        _meta.canExecute = has(std::filesystem::perms::owner_exec) || has(std::filesystem::perms::group_exec) || has(std::filesystem::perms::others_exec);
    } else {
        _meta.size = 0;
        _meta.canRead = _meta.canWrite = _meta.canExecute = false;
    }
    _meta.owner = std::nullopt;
}

FileOperationHandle FileHandle::readAll() const {
    ReadOptions opts;
    if (_backend) {
        return _backend->readFile(_meta.path, opts);
    }
    // Should not happen: VFS must attach a backend for FileHandle
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::readRange(uint64_t offset, size_t length) const {
    ReadOptions opts;
    opts.offset = offset;
    opts.length = length;
    if (_backend) {
        return _backend->readFile(_meta.path, opts);
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::readLine(size_t lineNumber) const {
    if (_backend) {
        return _backend->readLine(_meta.path, lineNumber);
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::readLineBinary(size_t lineNumber, std::byte delimiter) const {
    if (!_backend) {
        return FileOperationHandle::immediate(FileOpStatus::Failed);
    }
    auto backend = _backend;
    return _vfs->submit(_meta.path, [backend, lineNumber, delimiter](FileOperationHandle::OpState& s, const std::string& p){
        ReadOptions ro{}; ro.binary = true; // read all bytes
        auto rh = backend->readFile(p, ro);
        rh.wait();
        if (rh.status() != FileOpStatus::Complete && rh.status() != FileOpStatus::Partial) {
            // Surface backend error if any
            const auto& err = rh.errorInfo();
            s.setError(err.code == FileError::None ? FileError::IOError : err.code,
                       err.message.empty() ? std::string("Failed to read for readLineBinary") : err.message,
                       err.path);
            s.complete(FileOpStatus::Failed);
            return;
        }
        auto buf = rh.contentsBytes();
        size_t idx = 0;
        std::vector<std::byte> line;
        for (size_t i = 0; i < buf.size(); ++i) {
            if (buf[i] == delimiter) {
                if (idx == lineNumber) break; else { line.clear(); ++idx; continue; }
            }
            line.push_back(buf[i]);
        }
        if (idx != lineNumber) { s.complete(FileOpStatus::Partial); return; }
        s.bytes = std::move(line);
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::writeAll(std::span<const std::byte> bytes) const {
    if (_backend && _vfs) {
        WriteOptions opts; opts.truncate = true;
        auto data = std::vector<std::byte>(bytes.begin(), bytes.end());
        return _vfs->submitSerialized(_meta.path, [opts, data=std::move(data)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p) mutable {
            auto* localBackend = dynamic_cast<LocalFileSystemBackend*>(backend.get());
            if (localBackend) {
                localBackend->doWriteFile(s, p, std::span<const std::byte>(data.data(), data.size()), opts);
            } else {
                s.setError(FileError::Unknown, "Backend does not support synchronous operations", p);
                s.complete(FileOpStatus::Failed);
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::writeRange(uint64_t offset, std::span<const std::byte> bytes) const {
    WriteOptions opts; opts.offset = offset; opts.truncate = false;
    if (_backend && _vfs) {
        auto data = std::vector<std::byte>(bytes.begin(), bytes.end());
        return _vfs->submitSerialized(_meta.path, [opts, data=std::move(data)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p) mutable {
            auto* localBackend = dynamic_cast<LocalFileSystemBackend*>(backend.get());
            if (localBackend) {
                localBackend->doWriteFile(s, p, std::span<const std::byte>(data.data(), data.size()), opts);
            } else {
                s.setError(FileError::Unknown, "Backend does not support synchronous operations", p);
                s.complete(FileOpStatus::Failed);
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::writeLine(size_t lineNumber, std::string_view line) const {
    if (_backend && _vfs) {
        auto lineCopy = std::string(line);
        return _vfs->submitSerialized(_meta.path, [lineNumber, lineCopy=std::move(lineCopy)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p) mutable {
            auto* localBackend = dynamic_cast<LocalFileSystemBackend*>(backend.get());
            if (localBackend) {
                localBackend->doWriteLine(s, p, lineNumber, lineCopy);
            } else {
                s.setError(FileError::Unknown, "Backend does not support synchronous operations", p);
                s.complete(FileOpStatus::Failed);
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::writeAll(std::string_view text) const {
    if (_backend && _vfs) {
        WriteOptions opts; opts.truncate = true;
        auto textCopy = std::string(text);
        return _vfs->submitSerialized(_meta.path, [opts, textCopy=std::move(textCopy)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p) mutable {
            auto* localBackend = dynamic_cast<LocalFileSystemBackend*>(backend.get());
            if (localBackend) {
                auto spanBytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(textCopy.data()), textCopy.size());
                localBackend->doWriteFile(s, p, spanBytes, opts);
            } else {
                s.setError(FileError::Unknown, "Backend does not support synchronous operations", p);
                s.complete(FileOpStatus::Failed);
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::createEmpty() const {
    if (_backend && _vfs) {
        return _vfs->submitSerialized(_meta.path, [](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p) mutable {
            auto* localBackend = dynamic_cast<LocalFileSystemBackend*>(backend.get());
            if (localBackend) {
                localBackend->doCreateFile(s, p);
            } else {
                s.setError(FileError::Unknown, "Backend does not support synchronous operations", p);
                s.complete(FileOpStatus::Failed);
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::remove() const {
    if (_backend && _vfs) {
        return _vfs->submitSerialized(_meta.path, [](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p) mutable {
            auto* localBackend = dynamic_cast<LocalFileSystemBackend*>(backend.get());
            if (localBackend) {
                localBackend->doDeleteFile(s, p);
            } else {
                s.setError(FileError::Unknown, "Backend does not support synchronous operations", p);
                s.complete(FileOpStatus::Failed);
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

// Stream factory methods - FileHandle acts as factory
std::unique_ptr<FileStream> FileHandle::openReadStream() const {
    if (_vfs) {
        StreamOptions opts;
        opts.mode = StreamOptions::Read;
        return _vfs->openStream(_meta.path, opts);
    }
    return nullptr;
}

std::unique_ptr<FileStream> FileHandle::openWriteStream(bool append) const {
    (void)append; // append semantics can be handled by backend via options in the future
    if (_vfs) {
        StreamOptions opts;
        opts.mode = StreamOptions::Write;
        return _vfs->openStream(_meta.path, opts);
    }
    return nullptr;
}

std::unique_ptr<FileStream> FileHandle::openReadWriteStream() const {
    if (_vfs) {
        StreamOptions opts;
        opts.mode = StreamOptions::ReadWrite;
        return _vfs->openStream(_meta.path, opts);
    }
    return nullptr;
}

std::unique_ptr<BufferedFileStream> FileHandle::openBufferedStream(size_t bufferSize) const {
    if (_vfs) {
        StreamOptions opts;
        opts.mode = StreamOptions::ReadWrite;
        return _vfs->openBufferedStream(_meta.path, bufferSize, opts);
    }
    return nullptr;
}


} // namespace EntropyEngine::Core::IO
