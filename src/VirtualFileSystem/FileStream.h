#pragma once
#include <span>
#include <cstddef>
#include <ios>
#include <optional>
#include "FileOperationHandle.h"

namespace EntropyEngine::Core::IO {

// Result structure for I/O operations
struct IoResult {
    size_t bytesTransferred = 0;
    bool complete = false;
    std::optional<FileError> error;
    
    bool success() const { return !error.has_value(); }
};

// Pure interface for file streaming
class FileStream {
public:
    virtual ~FileStream() = default;
    
    // Core I/O operations
    // Read into buffer, returns actual bytes read
    virtual IoResult read(std::span<std::byte> buffer) = 0;
    
    // Write data, returns actual bytes written  
    virtual IoResult write(std::span<const std::byte> data) = 0;
    
    // Positioning
    virtual bool seek(int64_t offset, std::ios_base::seekdir dir = std::ios_base::beg) = 0;
    virtual int64_t tell() const = 0;
    
    // Stream state
    virtual bool good() const = 0;
    virtual bool eof() const = 0;
    virtual bool fail() const = 0;
    
    // Flush any buffered data
    virtual void flush() = 0;
    
    // Close the stream (called automatically by destructor)
    virtual void close() = 0;
    
    // Get underlying file path if applicable
    virtual std::string path() const { return ""; }
};

// Buffered stream wrapper - adds buffering to any stream
class BufferedFileStream : public FileStream {
public:
    BufferedFileStream(std::unique_ptr<FileStream> inner, size_t bufferSize = 8192);
    
    IoResult read(std::span<std::byte> buffer) override;
    IoResult write(std::span<const std::byte> data) override;
    bool seek(int64_t offset, std::ios_base::seekdir dir) override;
    int64_t tell() const override;
    bool good() const override;
    bool eof() const override;
    bool fail() const override;
    void flush() override;
    void close() override;
    std::string path() const override { return _inner->path(); }
    
private:
    std::unique_ptr<FileStream> _inner;
    std::vector<std::byte> _readBuffer;
    std::vector<std::byte> _writeBuffer;
    size_t _readPos = 0;
    size_t _readSize = 0;
    size_t _writePos = 0;
    bool _dirty = false;
    
    void flushWriteBuffer();
    void fillReadBuffer();
};

} // namespace EntropyEngine::Core::IO