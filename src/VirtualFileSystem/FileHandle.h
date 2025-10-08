/**
 * @file FileHandle.h
 * @brief Value-semantic handle for performing file operations through VFS
 * 
 * FileHandle provides a convenient, copyable reference to a file path routed through a
 * VirtualFileSystem backend. Use it to read/write text or bytes, manipulate lines, and
 * open streams. Equality and hashing are backend-aware via a normalized key.
 */
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
struct WriteOptions;      // fwd

/**
 * @brief Copyable handle to a file path routed through a backend
 * 
 * Construct via VirtualFileSystem::createFileHandle(). Operations are asynchronous;
 * call wait() on the returned FileOperationHandle to block, or chain operations.
 *
 * Design note: FileHandle is a dumb handle that delegates all I/O and policy decisions
 * to the routed backend through the VirtualFileSystem. It avoids filesystem probing and
 * contains no backend-specific logic; semantics are defined by the backend implementation.
 * 
 * @code
 * WorkContractGroup group(2000);
 * VirtualFileSystem vfs(&group);
 * auto fh = vfs.createFileHandle("example.txt");
 * fh.writeAll("Hello\n").wait();
 * auto r = fh.readAll(); r.wait();
 * ENTROPY_LOG_INFO(r.contentsText());
 * @endcode
 */
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

private:
    /**
     * @brief Constructs a handle bound to a VirtualFileSystem and path
     * 
     * Use VirtualFileSystem::createFileHandle() to obtain instances. The handle is copyable and
     * cheap to pass by value. Operations are asynchronous; call wait() on returned handles.
     * Only VirtualFileSystem may construct FileHandle to ensure a backend is attached.
     * @param vfs VFS instance that will execute operations
     * @param path Target file path (routed to a backend)
     */
    explicit FileHandle(VirtualFileSystem* vfs, std::string path);

public:

    // Reads
    /**
     * @brief Reads the entire file into memory
     * 
     * Asynchronously reads the full contents as bytes. Call contentsText() or contentsBytes()
     * on the returned handle after wait().
     * @return Handle representing the read operation
     * @code
     * auto h = fh.readAll(); h.wait();
     * if (h.status()==FileOpStatus::Complete) ENTROPY_LOG_INFO(h.contentsText());
     * @endcode
     */
    FileOperationHandle readAll() const;
    /**
     * @brief Reads a byte range from the file
     * @param offset Starting byte offset
     * @param length Number of bytes to read
     * @return Handle for the asynchronous read; status may be Partial if EOF reached early
     */
    FileOperationHandle readRange(uint64_t offset, size_t length) const;
    /**
     * @brief Reads a line by index (0-based), trimming line-ending
     * @param lineNumber Line index to read
     * @return Handle with contentsText() set to the line on success; Partial if index out of range
     */
    FileOperationHandle readLine(size_t lineNumber) const;
    /**
     * @brief Reads a line using a custom byte delimiter (binary-safe)
     * @param lineNumber Line index
     * @param delimiter Byte delimiter to split lines
     * @return Handle with raw bytes of the line
     */
    FileOperationHandle readLineBinary(size_t lineNumber, std::byte delimiter) const;

    // Writes
    /**
     * @brief Writes the full text to the file (overwrites by default)
     * 
     * Uses LF/CRLF policy as implemented by the backend. Use WriteBatch for line-wise edits.
     * @param text UTF-8 text to write
     * @return Handle for the asynchronous write
     */
    FileOperationHandle writeAll(std::string_view text) const;
    /**
     * @brief Writes full text with explicit WriteOptions
     */
    FileOperationHandle writeAll(std::string_view text, const WriteOptions& opts) const;
    /**
     * @brief Writes raw bytes to the file (overwrites by default)
     * @param bytes Data to write
     * @return Handle for the asynchronous write
     */
    FileOperationHandle writeAll(std::span<const std::byte> bytes) const;
    /**
     * @brief Writes raw bytes with explicit WriteOptions
     */
    FileOperationHandle writeAll(std::span<const std::byte> bytes, const WriteOptions& opts) const;
    /**
     * @brief Writes bytes starting at a specific offset
     * @param offset Byte offset to begin writing
     * @param bytes Data to write
     * @return Handle for the asynchronous write
     */
    FileOperationHandle writeRange(uint64_t offset, std::span<const std::byte> bytes) const;
    /**
     * @brief Writes bytes at offset with explicit WriteOptions (offset is applied to opts)
     */
    FileOperationHandle writeRange(uint64_t offset, std::span<const std::byte> bytes, const WriteOptions& opts) const;
    /**
     * @brief Replaces a single line by index (0-based)
     * 
     * Extends the file with blank lines if the index is beyond EOF. Line endings are preserved
     * according to backend policy.
     * @param lineNumber Line to overwrite
     * @param line New line content (without newline)
     * @return Handle for the asynchronous write
     */
    FileOperationHandle writeLine(size_t lineNumber, std::string_view line) const;
    /**
     * @brief Replaces a single line with explicit WriteOptions (currently forwarded)
     */
    FileOperationHandle writeLine(size_t lineNumber, std::string_view line, const WriteOptions& opts) const;

    // File management
    /**
     * @brief Creates an empty file or truncates existing to zero
     * @return Handle for the asynchronous creation/truncation
     */
    FileOperationHandle createEmpty() const; // create or truncate to zero length
    /**
     * @brief Deletes the file if it exists (idempotent)
     * @return Handle for the asynchronous delete operation
     */
    FileOperationHandle remove() const;      // delete file if exists (idempotent)
    
    // Streaming API - FileHandle acts as factory for streams
    /**
     * @brief Opens an unbuffered read-only stream
     * @return FileStream unique_ptr, or null on failure
     */
    std::unique_ptr<FileStream> openReadStream() const;
    /**
     * @brief Opens an unbuffered write-only stream
     * @param append If true, writes are appended; otherwise truncate/overwrite
     * @return FileStream unique_ptr, or null on failure
     */
    std::unique_ptr<FileStream> openWriteStream(bool append = false) const;
    /**
     * @brief Opens an unbuffered read-write stream
     * @return FileStream unique_ptr, or null on failure
     */
    std::unique_ptr<FileStream> openReadWriteStream() const;
    /**
     * @brief Opens a buffered stream wrapper around an unbuffered stream
     * @param bufferSize Size of the internal buffer in bytes
     * @return BufferedFileStream unique_ptr, or null on failure
     */
    std::unique_ptr<BufferedFileStream> openBufferedStream(size_t bufferSize = 65536) const;

    /**
     * @brief Returns static metadata captured at handle construction
     * @return Reference to file metadata (existence, size at creation time, etc.)
     */
    const Metadata& metadata() const noexcept { return _meta; }

    /**
     * @brief Backend-aware normalized key for identity/locking
     * @return Normalized key string used for equality and advisory locks
     */
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
