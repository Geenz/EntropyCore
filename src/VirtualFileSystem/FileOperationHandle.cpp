#include "FileOperationHandle.h"

namespace EntropyEngine::Core::IO {

void FileOperationHandle::wait() const {
    if (!_s) return;
    
    // Fast path - already complete
    if (_s->isComplete.load(std::memory_order_acquire)) {
        return;
    }
    
    // Slow path - wait for completion
    std::unique_lock<std::mutex> lock(_s->completionMutex);
    _s->completionCV.wait(lock, [this] {
        return _s->isComplete.load(std::memory_order_acquire);
    });
}

FileOpStatus FileOperationHandle::status() const noexcept {
    return _s ? _s->st.load(std::memory_order_acquire) : FileOpStatus::Failed;
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
    
    // Convert bytes to string on demand
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

} // namespace EntropyEngine::Core::IO
