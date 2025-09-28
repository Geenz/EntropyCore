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

namespace EntropyEngine::Core::IO {

enum class FileOpStatus { Pending, Running, Partial, Complete, Failed };

enum class FileError {
    None = 0,
    FileNotFound,
    AccessDenied,
    DiskFull,
    InvalidPath,
    IOError,
    NetworkError,
    Unknown
};

struct FileErrorInfo {
    FileError code = FileError::None;
    std::string message;
    std::optional<std::error_code> systemError;
    std::string path;
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
    
    // Error information - only valid after wait() and status is Failed
    const FileErrorInfo& errorInfo() const;

private:
    struct OpState {
        std::atomic<FileOpStatus> st{FileOpStatus::Pending};
        mutable std::mutex completionMutex;
        mutable std::condition_variable completionCV;
        std::atomic<bool> isComplete{false};
        
        // Result data - only valid after completion
        std::vector<std::byte> bytes;    // for reads
        uint64_t wrote = 0;              // for writes
        FileErrorInfo error;             // error details if failed
        
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
};

} // namespace EntropyEngine::Core::IO
