#include "LocalFileSystemBackend.h"
#include "VirtualFileSystem.h"
#include "FileStream.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cstring>
#include <random>

namespace EntropyEngine::Core::IO {

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

std::shared_ptr<std::mutex> LocalFileSystemBackend::getWriteLock(const std::string& path) {
    std::lock_guard<std::mutex> lock(_lockMapMutex);
    auto it = _writeLocks.find(path);
    if (it != _writeLocks.end()) {
        return it->second;
    }
    auto mutex = std::make_shared<std::mutex>();
    _writeLocks[path] = mutex;
    return mutex;
}

FileOperationHandle LocalFileSystemBackend::readFile(const std::string& path, ReadOptions options) {
    return submitWork(path, [options](FileOperationHandle::OpState& s, const std::string& p) {
        std::ifstream in(p, std::ios::in | std::ios::binary);
        if (!in) {
            s.setError(FileError::FileNotFound, "File not found or cannot be opened", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        
        if (options.offset > 0) {
            in.seekg(options.offset, std::ios::beg);
        }
        
        if (options.length.has_value()) {
            s.bytes.resize(options.length.value());
            in.read(reinterpret_cast<char*>(s.bytes.data()), options.length.value());
            s.bytes.resize(static_cast<size_t>(in.gcount()));
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
    auto lock = getWriteLock(path);
    
    return submitWork(path, [lock, data = std::vector<std::byte>(data.begin(), data.end()), options]
                            (FileOperationHandle::OpState& s, const std::string& p) mutable {
        std::unique_lock<std::mutex> pathLock(*lock);
        
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
            s.setError(FileError::AccessDenied, "Cannot open file for writing", p);
            s.complete(FileOpStatus::Failed);
            return;
        }
        
        if (options.offset > 0 && !options.append) {
            out.seekp(options.offset, std::ios::beg);
        }
        
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        s.wrote = data.size();
        
        if (!out.good()) {
            s.setError(FileError::IOError, "Write operation failed", p);
            s.complete(FileOpStatus::Failed);
        } else {
            s.complete(FileOpStatus::Complete);
        }
    });
}

FileOperationHandle LocalFileSystemBackend::deleteFile(const std::string& path) {
    auto lock = getWriteLock(path);
    
    return submitWork(path, [lock](FileOperationHandle::OpState& s, const std::string& p) {
        std::unique_lock<std::mutex> pathLock(*lock);
        
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
    auto lock = getWriteLock(path);
    
    return submitWork(path, [lock](FileOperationHandle::OpState& s, const std::string& p) {
        std::unique_lock<std::mutex> pathLock(*lock);
        
        std::ofstream out(p, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) {
            s.setError(FileError::AccessDenied, "Cannot create file", p);
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
        
        if (ec) {
            s.setError(FileError::IOError, "Cannot get file status", p, ec);
            s.complete(FileOpStatus::Failed);
            return;
        }
        
        // For now, just store existence in bytes (hack - should have proper metadata field)
        s.bytes.resize(1);
        s.bytes[0] = std::filesystem::exists(status) ? std::byte{1} : std::byte{0};
        s.complete(FileOpStatus::Complete);
    });
}

bool LocalFileSystemBackend::exists(const std::string& path) {
    return std::filesystem::exists(path);
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

FileOperationHandle LocalFileSystemBackend::listDirectory(const std::string& path) {
    return submitWork(path, [](FileOperationHandle::OpState& s, const std::string& p) {
        try {
            // Collect directory entries as strings (simplified for now)
            std::string listing;
            for (const auto& entry : std::filesystem::directory_iterator(p)) {
                listing += entry.path().filename().string();
                listing += "\n";
            }
            
            s.bytes.resize(listing.size());
            std::memcpy(s.bytes.data(), listing.data(), listing.size());
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
    auto lock = getWriteLock(path);
    
    return submitWork(path, [lock, lineNumber, line = std::string(line)]
                            (FileOperationHandle::OpState& s, const std::string& p) mutable {
        std::unique_lock<std::mutex> pathLock(*lock);
        
        std::error_code ec;
        
        // Generate temporary filename
        auto tempPath = std::filesystem::temp_directory_path(ec) / 
                       (std::filesystem::path(p).filename().string() + ".tmp" + std::to_string(std::random_device{}()));
        
        if (ec) {
            s.setError(FileError::IOError, "Failed to get temp directory", p, ec);
            s.complete(FileOpStatus::Failed);
            return;
        }
        
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
        
        // Atomic rename
        std::filesystem::rename(tempPath, p, ec);
        if (ec) {
            std::filesystem::remove(tempPath, ec);
            s.setError(FileError::IOError, "Failed to rename temp file", p, ec);
            s.complete(FileOpStatus::Failed);
            return;
        }
        
        s.wrote = line.size();
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