#pragma once
#include <string>
#include <vector>
#include <memory>
#include <span>
#include <optional>
#include <string_view>
#include <cstddef>
#include "FileOperationHandle.h"
#include "FileStream.h"

namespace EntropyEngine::Core::IO {

class VirtualFileSystem; // fwd
class IFileSystemBackend; // fwd

class FileHandle {
public:
    struct Metadata {
        std::string path;       // full path as provided
        std::string directory;  // parent directory (may be empty)
        std::string filename;   // file name with extension
        std::string extension;  // extension including leading dot if present
        bool exists = false;    // whether file exists at construction time
        uintmax_t size = 0;     // size in bytes if regular file, else 0
        bool canRead = false;   // readable by someone (owner/group/others)
        bool canWrite = false;  // writable by someone
        bool canExecute = false; // executable by someone
        std::optional<std::string> owner; // platform-specific; may be empty
    };

    explicit FileHandle(VirtualFileSystem* vfs, std::string path);

    // Reads
    FileOperationHandle readAll() const;
    FileOperationHandle readRange(uint64_t offset, size_t length) const;
    FileOperationHandle readLine(size_t lineNumber) const;
    FileOperationHandle readLineBinary(size_t lineNumber, std::byte delimiter) const;

    // Writes
    FileOperationHandle writeAll(std::string_view text) const;
    FileOperationHandle writeAll(std::span<const std::byte> bytes) const;
    FileOperationHandle writeRange(uint64_t offset, std::span<const std::byte> bytes) const;
    FileOperationHandle writeLine(size_t lineNumber, std::string_view line) const;

    // File management
    FileOperationHandle createEmpty() const; // create or truncate to zero length
    FileOperationHandle remove() const;      // delete file if exists (idempotent)
    
    // Streaming API - FileHandle acts as factory for streams
    std::unique_ptr<FileStream> openReadStream() const;
    std::unique_ptr<FileStream> openWriteStream(bool append = false) const;
    std::unique_ptr<FileStream> openReadWriteStream() const;
    std::unique_ptr<BufferedFileStream> openBufferedStream(size_t bufferSize = 65536) const;

    const Metadata& metadata() const noexcept { return _meta; }

    // Normalized key for identity and locking (backend-aware)
    const std::string& normalizedKey() const noexcept { return _normKey; }

    // Equality based on backend identity and normalized key
    friend bool operator==(const FileHandle& a, const FileHandle& b) noexcept {
        return a._backend.get() == b._backend.get() && a._normKey == b._normKey;
    }
    friend bool operator!=(const FileHandle& a, const FileHandle& b) noexcept { return !(a == b); }

private:
    VirtualFileSystem* _vfs;
    std::shared_ptr<IFileSystemBackend> _backend;  // Backend for this file (ref-counted for safety)
    Metadata _meta; // associated metadata for this handle
    std::string _normKey; // backend-normalized key captured at creation

    friend class VirtualFileSystem;
};

} // namespace EntropyEngine::Core::IO

// Hash support for FileHandle
namespace std {
    template<>
    struct hash<EntropyEngine::Core::IO::FileHandle> {
        size_t operator()(const EntropyEngine::Core::IO::FileHandle& h) const noexcept {
            // Combine backend pointer and normalized key
            // Note: To avoid accessing private members, hash only the normalized key.
            // This satisfies the requirement that equal objects have equal hashes, though it may increase collisions across backends.
            size_t seed = std::hash<std::string>{}(h.normalizedKey());
            return seed;
        }
    };
}
