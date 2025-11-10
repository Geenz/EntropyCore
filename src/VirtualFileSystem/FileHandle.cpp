#include "FileHandle.h"
#include "VirtualFileSystem.h"
#include "IFileSystemBackend.h"
#include "LocalFileSystemBackend.h"
#include "FileStream.h"
#include <filesystem>
#include <vector>
#include <string>

using EntropyEngine::Core::Concurrency::ExecutionType;

namespace EntropyEngine::Core::IO {


FileHandle::FileHandle(VirtualFileSystem* vfs, std::string path)
    : _vfs(vfs) {
    _meta.path = std::move(path);
    std::filesystem::path pp(_meta.path);
    _meta.directory = pp.has_parent_path() ? pp.parent_path().string() : std::string();
    _meta.filename = pp.filename().string();
    _meta.extension = pp.has_extension() ? pp.extension().string() : std::string();
    // Dumb handle: no filesystem probing here. Defer to backend for metadata when requested.
    _meta.exists = false;
    _meta.size = 0;
    _meta.canRead = _meta.canWrite = _meta.canExecute = false;
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

FileOperationHandle FileHandle::readLineBinary(size_t lineNumber, uint8_t delimiter) const {
    if (!_backend) {
        return FileOperationHandle::immediate(FileOpStatus::Failed);
    }
    auto backend = _backend;
    return _vfs->submit(_meta.path, [backend, lineNumber, delimiter](FileOperationHandle::OpState& s, const std::string& p, const ExecContext&){
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
        std::vector<uint8_t> line;
        for (size_t i = 0; i < buf.size(); ++i) {
            if (buf[i] == delimiter) {
                if (idx == lineNumber) break; else { line.clear(); ++idx; continue; }
            }
            line.push_back(buf[i]);
        }
        if (idx != lineNumber) { s.complete(FileOpStatus::Partial); return; }
        s.bytes.assign(line.begin(), line.end());
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle FileHandle::writeAll(std::span<const uint8_t> bytes) const {
    if (_backend && _vfs) {
        WriteOptions opts; opts.truncate = true;
        auto data = std::vector<uint8_t>(bytes.begin(), bytes.end());
        return _vfs->submitSerialized(_meta.path, [opts, data=std::move(data)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p, const ExecContext&) mutable {
            auto byteSpan = std::span<const uint8_t>(data.data(), data.size());
            auto inner = backend->writeFile(p, byteSpan, opts);
            inner.wait();
            auto st = inner.status();
            if (st == FileOpStatus::Complete || st == FileOpStatus::Partial) {
                s.wrote = inner.bytesWritten();
                s.complete(st);
            } else {
                const auto& err = inner.errorInfo();
                s.setError(err.code == FileError::None ? FileError::IOError : err.code,
                           err.message, err.path, err.systemError);
                s.complete(FileOpStatus::Failed);
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::writeAll(std::span<const uint8_t> bytes, const WriteOptions& opts) const {
    if (_backend && _vfs) {
        auto data = std::vector<uint8_t>(bytes.begin(), bytes.end());
        return _vfs->submitSerialized(_meta.path, [opts, data=std::move(data)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p, const ExecContext&) mutable {
            auto byteSpan = std::span<const uint8_t>(data.data(), data.size());
            if (auto* local = dynamic_cast<LocalFileSystemBackend*>(backend.get())) {
                local->doWriteFile(s, p, byteSpan, opts);
            } else {
                auto inner = backend->writeFile(p, byteSpan, opts);
                inner.wait();
                auto st = inner.status();
                if (st == FileOpStatus::Complete || st == FileOpStatus::Partial) {
                    s.wrote = inner.bytesWritten();
                    s.complete(st);
                } else {
                    const auto& err = inner.errorInfo();
                    s.setError(err.code == FileError::None ? FileError::IOError : err.code,
                               err.message, err.path, err.systemError);
                    s.complete(FileOpStatus::Failed);
                }
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::writeRange(uint64_t offset, std::span<const uint8_t> bytes) const {
    WriteOptions opts; opts.offset = offset; opts.truncate = false;
    if (_backend && _vfs) {
        auto data = std::vector<uint8_t>(bytes.begin(), bytes.end());
        return _vfs->submitSerialized(_meta.path, [opts, data=std::move(data)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p, const ExecContext&) mutable {
            auto byteSpan = std::span<const uint8_t>(data.data(), data.size());
            if (auto* local = dynamic_cast<LocalFileSystemBackend*>(backend.get())) {
                local->doWriteFile(s, p, byteSpan, opts);
            } else {
                auto inner = backend->writeFile(p, byteSpan, opts);
                inner.wait();
                auto st = inner.status();
                if (st == FileOpStatus::Complete || st == FileOpStatus::Partial) {
                    s.wrote = inner.bytesWritten();
                    s.complete(st);
                } else {
                    const auto& err = inner.errorInfo();
                    s.setError(err.code == FileError::None ? FileError::IOError : err.code,
                               err.message, err.path, err.systemError);
                    s.complete(FileOpStatus::Failed);
                }
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::writeRange(uint64_t offset, std::span<const uint8_t> bytes, const WriteOptions& opts) const {
    if (_backend && _vfs) {
        WriteOptions wopts = opts;
        wopts.offset = offset;
        wopts.truncate = false;
        auto data = std::vector<uint8_t>(bytes.begin(), bytes.end());
        return _vfs->submitSerialized(_meta.path, [wopts, data=std::move(data)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p, const ExecContext&) mutable {
            auto byteSpan = std::span<const uint8_t>(data.data(), data.size());
            if (auto* local = dynamic_cast<LocalFileSystemBackend*>(backend.get())) {
                local->doWriteFile(s, p, byteSpan, wopts);
            } else {
                auto inner = backend->writeFile(p, byteSpan, wopts);
                inner.wait();
                auto st = inner.status();
                if (st == FileOpStatus::Complete || st == FileOpStatus::Partial) {
                    s.wrote = inner.bytesWritten();
                    s.complete(st);
                } else {
                    const auto& err = inner.errorInfo();
                    s.setError(err.code == FileError::None ? FileError::IOError : err.code,
                               err.message, err.path, err.systemError);
                    s.complete(FileOpStatus::Failed);
                }
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::writeLine(size_t lineNumber, std::string_view line) const {
    if (_backend && _vfs) {
        auto lineCopy = std::string(line);
        return _vfs->submitSerialized(_meta.path, [lineNumber, lineCopy=std::move(lineCopy)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p, const ExecContext&) mutable {
            if (auto* local = dynamic_cast<LocalFileSystemBackend*>(backend.get())) {
                local->doWriteLine(s, p, lineNumber, lineCopy);
            } else {
                auto inner = backend->writeLine(p, lineNumber, lineCopy);
                inner.wait();
                auto st = inner.status();
                if (st == FileOpStatus::Complete || st == FileOpStatus::Partial) {
                    s.wrote = inner.bytesWritten();
                    s.complete(st);
                } else {
                    const auto& err = inner.errorInfo();
                    s.setError(err.code == FileError::None ? FileError::IOError : err.code,
                               err.message, err.path, err.systemError);
                    s.complete(FileOpStatus::Failed);
                }
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::writeAll(std::string_view text) const {
    if (_backend && _vfs) {
        WriteOptions opts; opts.truncate = true;
        auto textCopy = std::string(text);
        return _vfs->submitSerialized(_meta.path, [opts, textCopy=std::move(textCopy)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p, const ExecContext&) mutable {
            auto spanBytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(textCopy.data()), textCopy.size());
            if (auto* local = dynamic_cast<LocalFileSystemBackend*>(backend.get())) {
                local->doWriteFile(s, p, spanBytes, opts);
            } else {
                auto inner = backend->writeFile(p, spanBytes, opts);
                inner.wait();
                auto st = inner.status();
                if (st == FileOpStatus::Complete || st == FileOpStatus::Partial) {
                    s.wrote = inner.bytesWritten();
                    s.complete(st);
                } else {
                    const auto& err = inner.errorInfo();
                    s.setError(err.code == FileError::None ? FileError::IOError : err.code,
                               err.message, err.path, err.systemError);
                    s.complete(FileOpStatus::Failed);
                }
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::writeAll(std::string_view text, const WriteOptions& opts) const {
    if (_backend && _vfs) {
        auto textCopy = std::string(text);
        return _vfs->submitSerialized(_meta.path, [opts, textCopy=std::move(textCopy)](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p, const ExecContext&) mutable {
            auto spanBytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(textCopy.data()), textCopy.size());
            if (auto* local = dynamic_cast<LocalFileSystemBackend*>(backend.get())) {
                local->doWriteFile(s, p, spanBytes, opts);
            } else {
                auto inner = backend->writeFile(p, spanBytes, opts);
                inner.wait();
                auto st = inner.status();
                if (st == FileOpStatus::Complete || st == FileOpStatus::Partial) {
                    s.wrote = inner.bytesWritten();
                    s.complete(st);
                } else {
                    const auto& err = inner.errorInfo();
                    s.setError(err.code == FileError::None ? FileError::IOError : err.code,
                               err.message, err.path, err.systemError);
                    s.complete(FileOpStatus::Failed);
                }
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::writeLine(size_t lineNumber, std::string_view line, const WriteOptions& /*opts*/) const {
    // Currently forwards to default writeLine; WriteOptions are ignored for line-oriented writes.
    // Future: route via WriteBatch::commit(opts) for per-op control.
    return writeLine(lineNumber, line);
}

FileOperationHandle FileHandle::createEmpty() const {
    if (_backend && _vfs) {
        return _vfs->submitSerialized(_meta.path, [](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p, const ExecContext&) mutable {
            if (auto* local = dynamic_cast<LocalFileSystemBackend*>(backend.get())) {
                local->doCreateFile(s, p);
            } else {
                auto inner = backend->createFile(p);
                inner.wait();
                auto st = inner.status();
                if (st == FileOpStatus::Complete || st == FileOpStatus::Partial) {
                    s.complete(st);
                } else {
                    const auto& err = inner.errorInfo();
                    s.setError(err.code == FileError::None ? FileError::IOError : err.code,
                               err.message, err.path, err.systemError);
                    s.complete(FileOpStatus::Failed);
                }
            }
        });
    }
    return FileOperationHandle::immediate(FileOpStatus::Failed);
}

FileOperationHandle FileHandle::remove() const {
    if (_backend && _vfs) {
        return _vfs->submitSerialized(_meta.path, [](FileOperationHandle::OpState& s, std::shared_ptr<IFileSystemBackend> backend, const std::string& p, const ExecContext&) mutable {
            if (auto* local = dynamic_cast<LocalFileSystemBackend*>(backend.get())) {
                local->doDeleteFile(s, p);
            } else {
                auto inner = backend->deleteFile(p);
                inner.wait();
                auto st = inner.status();
                if (st == FileOpStatus::Complete || st == FileOpStatus::Partial) {
                    s.complete(st);
                } else {
                    const auto& err = inner.errorInfo();
                    s.setError(err.code == FileError::None ? FileError::IOError : err.code,
                               err.message, err.path, err.systemError);
                    s.complete(FileOpStatus::Failed);
                }
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
