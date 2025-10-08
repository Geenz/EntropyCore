#pragma once
#include <memory>
#include <vector>
#include <string>
#include <span>
#include <atomic>
#include <latch>
#include <mutex>
#include <condition_variable>
#include <string_view>
#include <cstddef>
#include <optional>
#include <system_error>
#include <chrono>
#include <functional>

namespace EntropyEngine::Core::IO {

enum class FileOpStatus { Pending, Running, Partial, Complete, Failed };

/**
 * Public error taxonomy surfaced by VFS operations.
 * Mapping guidelines:
 * - FileNotFound: path does not exist when required (read/stat)
 * - AccessDenied: open/create denied by OS/permissions
 * - DiskFull: ENOSPC/EDQUOT or equivalent on write/flush
 * - InvalidPath: malformed path, name too long, or parent missing
 * - IOError: other local I/O failures (including fsync failures)
 * - NetworkError: remote/backend transport failure (future backends)
 * - Timeout: bounded waits exceeded (advisory lock or backend scope)
 * - Conflict: contention detected (backend Busy without fallback)
 */
enum class FileError {
    None = 0,
    FileNotFound,
    AccessDenied,
    DiskFull,
    InvalidPath,
    IOError,
    NetworkError,
    Timeout,
    Conflict,
    Unknown
};

struct FileErrorInfo {
    FileError code = FileError::None;
    std::string message;
    std::optional<std::error_code> systemError;
    std::string path;
};

// File metadata - defined here so OpState can use it
struct FileMetadata {
    std::string path;
    bool exists = false;
    bool isDirectory = false;
    bool isRegularFile = false;
    bool isSymlink = false;
    uintmax_t size = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    std::optional<std::chrono::system_clock::time_point> lastModified;
    std::optional<std::string> mimeType;
};

// Directory entry with metadata - defined here so OpState can use it
struct DirectoryEntry {
    std::string name;           // Just the filename, not full path
    std::string fullPath;       // Complete absolute path
    FileMetadata metadata;      // Full metadata for this entry
    bool isSymlink = false;
    std::optional<std::string> symlinkTarget;
};

class FileOperationHandle {
public:
    FileOperationHandle() = default;

    void wait() const;
    FileOpStatus status() const noexcept;

    // Read results (views) - only valid after wait()
    std::span<const std::byte> contentsBytes() const;
    std::string contentsText() const;

    // Write results - only valid after wait()
    uint64_t bytesWritten() const;

    // Metadata results - only valid after wait()
    const std::optional<FileMetadata>& metadata() const;

    // Directory listing results - only valid after wait()
    const std::vector<DirectoryEntry>& directoryEntries() const;

    // Batch metadata results - only valid after wait()
    const std::vector<FileMetadata>& metadataBatch() const;

    // Error information - only valid after wait() and status is Failed
    const FileErrorInfo& errorInfo() const;

    // Factory for immediate completion (no async work needed)
    static FileOperationHandle immediate(FileOpStatus status);

private:
    struct OpState {
        std::atomic<FileOpStatus> st{FileOpStatus::Pending};
        mutable std::mutex completionMutex;
        mutable std::condition_variable completionCV;
        std::atomic<bool> isComplete{false};

        // Optional progress hook called by wait() to ensure forward progress
        std::function<void()> progress;

        // Result data - only valid after completion
        std::vector<std::byte> bytes;    // for reads
        uint64_t wrote = 0;              // for writes
        FileErrorInfo error;             // error details if failed
        std::string text;                // for text preview/read operations
        std::optional<FileMetadata> metadata;  // for metadata queries
        std::vector<DirectoryEntry> directoryEntries;  // for directory listings
        std::vector<FileMetadata> metadataBatch;  // for batch metadata queries
        
        void complete(FileOpStatus final) noexcept {
            {
                std::lock_guard<std::mutex> lock(completionMutex);
                st.store(final, std::memory_order_release);
                isComplete.store(true, std::memory_order_release);
            }
            completionCV.notify_all();
        }
        
        void setError(FileError code, const std::string& msg, 
                     const std::string& path = "",
                     std::optional<std::error_code> ec = std::nullopt) {
            error.code = code;
            error.message = msg;
            error.path = path;
            error.systemError = ec;
        }
    };

    std::shared_ptr<OpState> _s;
    explicit FileOperationHandle(std::shared_ptr<OpState> s) : _s(std::move(s)) {}

    friend class VirtualFileSystem;
    friend class FileHandle;
    friend class LocalFileSystemBackend;
    friend class WriteBatch;
    friend class FileWatchManager;
};

} // namespace EntropyEngine::Core::IO
