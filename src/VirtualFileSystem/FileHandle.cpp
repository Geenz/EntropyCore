#include "FileHandle.h"
#include "VirtualFileSystem.h"
#include "IFileSystemBackend.h"
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
    // Use backend if available
    if (_backend) {
        ReadOptions opts;
        return _backend->readFile(_meta.path, opts);
    }
    
    // Fallback to direct implementation
    return _vfs->submit(_meta.path, [](FileOperationHandle::OpState& s, const std::string& p){
        std::ifstream in(p, std::ios::in | std::ios::binary);
        if (!in) {
            s.setError(FileError::FileNotFound, "File not found or cannot be opened", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        
        // Get file size
        in.seekg(0, std::ios::end);
        auto sizePos = in.tellg();
        if (sizePos < 0) {
            s.setError(FileError::IOError, "Failed to get file size", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        auto size = static_cast<std::streamsize>(sizePos);
        in.seekg(0, std::ios::beg);
        
        // Read directly into bytes
        s.bytes.resize(static_cast<size_t>(size));
        if (size > 0) {
            in.read(reinterpret_cast<char*>(s.bytes.data()), size);
        }
        
        if (!in.good() && !in.eof()) {
            s.setError(FileError::IOError, "Failed to read file", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::readRange(uint64_t offset, size_t length) const {
    // Use backend if available
    if (_backend) {
        ReadOptions opts;
        opts.offset = offset;
        opts.length = length;
        return _backend->readFile(_meta.path, opts);
    }
    
    // Fallback to direct implementation
    return _vfs->submit(_meta.path, [offset, length](FileOperationHandle::OpState& s, const std::string& p){
        std::ifstream in(p, std::ios::in | std::ios::binary);
        if (!in) {
            s.setError(FileError::FileNotFound, "File not found or cannot be opened", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        in.seekg(0, std::ios::end);
        auto endPos = in.tellg();
        if (endPos < 0) {
            s.setError(FileError::IOError, "Failed to get file size", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        auto end = static_cast<uint64_t>(static_cast<std::make_unsigned_t<std::streamoff>>(endPos));
        if (offset > end) {
            s.bytes.clear();
            s.complete(FileOpStatus::Partial);
            return;
        }
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        size_t toRead = static_cast<size_t>(std::min<uint64_t>(length, end - offset));
        s.bytes.resize(toRead);
        if (toRead > 0) {
            in.read(reinterpret_cast<char*>(s.bytes.data()), static_cast<std::streamsize>(toRead));
        }
        size_t got = static_cast<size_t>(in.gcount());
        s.bytes.resize(got);
        s.complete(got < length ? FileOpStatus::Partial : FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::readLine(size_t lineNumber) const {
    // Use backend if available
    if (_backend) {
        return _backend->readLine(_meta.path, lineNumber);
    }
    
    // Fallback to direct implementation
    return _vfs->submit(_meta.path, [lineNumber](FileOperationHandle::OpState& s, const std::string& p){
        std::ifstream in(p, std::ios::in);
        if (!in) {
            s.setError(FileError::FileNotFound, "File not found or cannot be opened", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        std::string line;
        size_t idx = 0;
        while (idx <= lineNumber && std::getline(in, line)) {
            if (idx == lineNumber) break;
            ++idx;
        }
        if (idx != lineNumber) {
            s.complete(FileOpStatus::Partial);
            return;
        }
        // Strip trailing CR for Windows CRLF if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        s.bytes.resize(line.size());
        std::memcpy(s.bytes.data(), line.data(), line.size());
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::readLineBinary(size_t lineNumber, std::byte delimiter) const {
    return _vfs->submit(_meta.path, [lineNumber, delimiter](FileOperationHandle::OpState& s, const std::string& p){
        std::ifstream in(p, std::ios::in | std::ios::binary);
        if (!in) {
            s.setError(FileError::FileNotFound, "File not found or cannot be opened", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        size_t idx = 0;
        std::vector<std::byte> line;
        int c;
        while (idx <= lineNumber && (c = in.get()) != EOF) {
            if (static_cast<std::byte>(c) == delimiter) {
                if (idx == lineNumber) break; else { line.clear(); ++idx; continue; }
            }
            line.push_back(static_cast<std::byte>(c));
        }
        if (idx != lineNumber) {
            s.complete(FileOpStatus::Partial);
            return;
        }
        s.bytes = std::move(line);
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::writeAll(std::span<const std::byte> bytes) const {
    // Use backend if available
    if (_backend) {
        WriteOptions opts;
        opts.truncate = true;
        return _backend->writeFile(_meta.path, bytes, opts);
    }
    
    // Fallback to direct implementation
    auto lock = _vfs->lockForPath(_meta.path);
    return _vfs->submit(_meta.path, [lock, copy=std::vector<std::byte>(bytes.begin(), bytes.end())](FileOperationHandle::OpState& s, const std::string& p) mutable {
        std::unique_lock<std::mutex> pathLock; if (lock) pathLock = std::unique_lock<std::mutex>(*lock);
        std::ofstream out(p, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) { 
            s.setError(FileError::AccessDenied, "Cannot create or write to file", p);
            s.complete(FileOpStatus::Failed); 
            return; 
        }
        if (!copy.empty()) out.write(reinterpret_cast<const char*>(copy.data()), static_cast<std::streamsize>(copy.size()));
        out.flush();
        if (!out.good()) {
            s.setError(FileError::IOError, "Failed to flush file (disk full?)", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        s.wrote = copy.size();
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::writeRange(uint64_t offset, std::span<const std::byte> bytes) const {
    // Use backend if available
    if (_backend) {
        WriteOptions opts;
        opts.offset = offset;
        opts.truncate = false;
        return _backend->writeFile(_meta.path, bytes, opts);
    }
    
    // Fallback to direct implementation
    auto lock = _vfs->lockForPath(_meta.path);
    return _vfs->submit(_meta.path, [lock, offset, copy=std::vector<std::byte>(bytes.begin(), bytes.end())](FileOperationHandle::OpState& s, const std::string& p) mutable {
        std::unique_lock<std::mutex> pathLock; if (lock) pathLock = std::unique_lock<std::mutex>(*lock);
        std::fstream io(p, std::ios::in | std::ios::out | std::ios::binary);
        if (!io) { // try create
            io = std::fstream(p, std::ios::out | std::ios::binary);
            io.close();
            io.open(p, std::ios::in | std::ios::out | std::ios::binary);
        }
        if (!io) { 
            s.setError(FileError::IOError, "Cannot open file for writing", p);
            s.complete(FileOpStatus::Failed); 
            return; 
        }
        io.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!copy.empty()) io.write(reinterpret_cast<const char*>(copy.data()), static_cast<std::streamsize>(copy.size()));
        io.flush();
        if (!io.good()) {
            s.setError(FileError::IOError, "Failed to flush file (disk full?)", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        s.wrote = copy.size();
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::writeLine(size_t lineNumber, std::string_view line) const {
    // Use backend if available
    if (_backend) {
        return _backend->writeLine(_meta.path, lineNumber, line);
    }
    
    // Fallback to direct implementation
    auto lock = _vfs->lockForPath(_meta.path);
    return _vfs->submit(_meta.path, [lock, lineNumber, line = std::string(line)](FileOperationHandle::OpState& s, const std::string& p) mutable {
        std::unique_lock<std::mutex> pathLock; if (lock) pathLock = std::unique_lock<std::mutex>(*lock);
        
        std::error_code ec;
        
        // Generate temporary filename in the same directory as destination
        auto targetPath = std::filesystem::path(p);
        auto dir = targetPath.parent_path();
        auto base = targetPath.filename().string();
        auto tempPath = dir / (base + ".tmp" + std::to_string(std::random_device{}()));
        
        // Read from original and write to temp file
        {
            std::ifstream in(p, std::ios::in);
            std::ofstream out(tempPath, std::ios::out | std::ios::trunc);
            
            if (!out) {
                s.setError(FileError::IOError, "Failed to create temp file", tempPath.string());
                s.complete(FileOpStatus::Failed);
                return;
            }
            
            std::string currentLine;
            size_t currentLineNum = 0;
            bool fileExists = in.is_open();
            
            // Process existing lines or create new file
            if (fileExists) {
                while (std::getline(in, currentLine)) {
                    if (currentLineNum == lineNumber) {
                        out << line;
                    } else {
                        out << currentLine;
                    }
                    out << "\n";  // Always add newline after each line
                    currentLineNum++;
                }
            }
            
            // Add empty lines if needed
            while (currentLineNum < lineNumber) {
                out << "\n";
                currentLineNum++;
            }
            
            // Add the target line if we haven't yet
            if (currentLineNum == lineNumber) {
                out << line << "\n";  // Add newline after the new line too
                currentLineNum++;
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
        ec.clear();
    #if defined(_WIN32)
        if (!win_replace_file(tempPath, targetPath)) {
            std::filesystem::remove(tempPath, ec);
            s.setError(FileError::IOError, "Failed to replace destination file", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
    #else
        std::filesystem::rename(tempPath, targetPath, ec);
        if (ec) {
            std::filesystem::remove(tempPath, ec);  // Clean up temp file
            s.setError(FileError::IOError, "Failed to rename temp file", p, ec);
            s.complete(FileOpStatus::Failed);
            return;
        }
    #endif
        
        s.wrote = line.size();
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::writeAll(std::string_view text) const {
    // Use backend if available
    if (_backend) {
        WriteOptions opts;
        opts.truncate = true;
        return _backend->writeFile(_meta.path, 
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()), opts);
    }
    
    // Fallback to direct implementation
    auto lock = _vfs->lockForPath(_meta.path);
    return _vfs->submit(_meta.path, [lock, txt = std::string(text)](FileOperationHandle::OpState& s, const std::string& p) mutable {
        std::unique_lock<std::mutex> pathLock; if (lock) pathLock = std::unique_lock<std::mutex>(*lock);
        std::ofstream out(p, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) { 
            s.setError(FileError::AccessDenied, "Cannot create or write to file", p);
            s.complete(FileOpStatus::Failed); 
            return; 
        }
        if (!txt.empty()) out.write(txt.data(), static_cast<std::streamsize>(txt.size()));
        out.flush();
        if (!out.good()) {
            s.setError(FileError::IOError, "Failed to flush file (disk full?)", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        s.wrote = txt.size();
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::createEmpty() const {
    // Use backend if available
    if (_backend) {
        return _backend->createFile(_meta.path);
    }
    
    // Fallback to direct implementation
    auto lock = _vfs->lockForPath(_meta.path);
    return _vfs->submit(_meta.path, [lock](FileOperationHandle::OpState& s, const std::string& p) mutable {
        std::unique_lock<std::mutex> pathLock; if (lock) pathLock = std::unique_lock<std::mutex>(*lock);
        std::ofstream out(p, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) { 
            s.setError(FileError::AccessDenied, "Cannot create file", p);
            s.complete(FileOpStatus::Failed); 
            return; 
        }
        out.flush();
        if (!out.good()) {
            s.setError(FileError::IOError, "Failed to create/truncate file", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        s.wrote = 0;
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::remove() const {
    // Use backend if available
    if (_backend) {
        return _backend->deleteFile(_meta.path);
    }
    
    // Fallback to direct implementation
    auto lock = _vfs->lockForPath(_meta.path);
    return _vfs->submit(_meta.path, [lock](FileOperationHandle::OpState& s, const std::string& p) mutable {
        std::unique_lock<std::mutex> pathLock; if (lock) pathLock = std::unique_lock<std::mutex>(*lock);
        std::error_code ec;
        (void)std::filesystem::remove(p, ec);
        if (ec) { 
            s.setError(FileError::IOError, "Failed to remove file", p, ec);
            s.complete(FileOpStatus::Failed); 
            return; 
        }
        s.wrote = 0;
        s.complete(FileOpStatus::Complete);
    });
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
