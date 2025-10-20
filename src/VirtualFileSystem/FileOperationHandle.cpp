#include "FileOperationHandle.h"
#include "IFileSystemBackend.h"
#include <chrono>

namespace EntropyEngine::Core::IO {

void FileOperationHandle::wait() const {
    if (!_s) return;
    
    // Fast path - already complete
    if (_s->isComplete.load(std::memory_order_acquire)) {
        return;
    }
    
    // Slow path - wait for completion with cooperative progress pumping
    std::unique_lock<std::mutex> lock(_s->completionMutex);
    while (!_s->isComplete.load(std::memory_order_acquire)) {
        // Opportunistically execute ready work while waiting
        lock.unlock();
        try {
            if (_s->progress) {
                _s->progress();
            }
        } catch (...) {
            // Swallow exceptions in progress to avoid breaking wait semantics
        }
        lock.lock();
        _s->completionCV.wait_for(lock, std::chrono::milliseconds(1), [this]{
            return _s->isComplete.load(std::memory_order_acquire);
        });
    }
}

FileOpStatus FileOperationHandle::status() const noexcept {
    return _s ? _s->st.load(std::memory_order_acquire) : FileOpStatus::Pending;
}

std::span<const std::byte> FileOperationHandle::contentsBytes() const {
    if (!_s) return {};
    
    // Ensure operation is complete before accessing data
    if (!_s->isComplete.load(std::memory_order_acquire)) {
        wait();
    }
    
    return std::span<const std::byte>(_s->bytes.data(), _s->bytes.size());
}

std::string FileOperationHandle::contentsText() const {
    if (!_s) return {};

    // Ensure operation is complete before accessing data
    if (!_s->isComplete.load(std::memory_order_acquire)) {
        wait();
    }

    // If text field is populated, use it (from preview operations)
    if (!_s->text.empty()) {
        return _s->text;
    }

    // Otherwise convert bytes to string on demand
    return std::string(reinterpret_cast<const char*>(_s->bytes.data()), _s->bytes.size());
}

uint64_t FileOperationHandle::bytesWritten() const {
    if (!_s) return 0ULL;
    
    // Ensure operation is complete before accessing data
    if (!_s->isComplete.load(std::memory_order_acquire)) {
        wait();
    }
    
    return _s->wrote;
}

const FileErrorInfo& FileOperationHandle::errorInfo() const {
    static FileErrorInfo emptyError;
    if (!_s) return emptyError;

    // Ensure operation is complete before accessing error info
    if (!_s->isComplete.load(std::memory_order_acquire)) {
        wait();
    }

    return _s->error;
}

const std::optional<FileMetadata>& FileOperationHandle::metadata() const {
    static const std::optional<FileMetadata> empty;
    if (!_s) return empty;

    // Ensure operation is complete before accessing metadata
    if (!_s->isComplete.load(std::memory_order_acquire)) {
        wait();
    }

    return _s->metadata;
}

const std::vector<DirectoryEntry>& FileOperationHandle::directoryEntries() const {
    static const std::vector<DirectoryEntry> empty;
    if (!_s) return empty;

    // Ensure operation is complete before accessing directory entries
    if (!_s->isComplete.load(std::memory_order_acquire)) {
        wait();
    }

    return _s->directoryEntries;
}

const std::vector<FileMetadata>& FileOperationHandle::metadataBatch() const {
    static const std::vector<FileMetadata> empty;
    if (!_s) return empty;

    // Ensure operation is complete before accessing batch metadata
    if (!_s->isComplete.load(std::memory_order_acquire)) {
        wait();
    }

    return _s->metadataBatch;
}

FileOperationHandle FileOperationHandle::immediate(FileOpStatus status) {
    auto state = std::make_shared<OpState>();
    state->st.store(status, std::memory_order_release);
    state->isComplete.store(true, std::memory_order_release);
    return FileOperationHandle(state);
}

std::shared_ptr<FileOperationHandle::OpState> FileOperationHandle::makeState() {
    return std::make_shared<OpState>();
}

FileOperationHandle::FileOperationHandle(std::shared_ptr<OpState> s) : _s(std::move(s)) {}

} // namespace EntropyEngine::Core::IO
