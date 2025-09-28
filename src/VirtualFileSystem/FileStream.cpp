#include "FileStream.h"
#include <algorithm>
#include <cstring>

namespace EntropyEngine::Core::IO {

BufferedFileStream::BufferedFileStream(std::unique_ptr<FileStream> inner, size_t bufferSize)
    : _inner(std::move(inner))
    , _readBuffer(bufferSize)
    , _writeBuffer(bufferSize) {
}

IoResult BufferedFileStream::read(std::span<std::byte> buffer) {
    IoResult result;
    
    if (!good()) {
        result.error = FileError::IOError;
        return result;
    }
    
    size_t totalRead = 0;
    size_t remaining = buffer.size();
    
    while (remaining > 0) {
        // If read buffer is empty, fill it
        if (_readPos >= _readSize) {
            fillReadBuffer();
            if (_readSize == 0) {
                // No more data available
                break;
            }
        }
        
        // Copy from read buffer to output
        size_t available = _readSize - _readPos;
        size_t toCopy = std::min(available, remaining);
        
        std::memcpy(buffer.data() + totalRead, 
                   _readBuffer.data() + _readPos, 
                   toCopy);
        
        _readPos += toCopy;
        totalRead += toCopy;
        remaining -= toCopy;
    }
    
    result.bytesTransferred = totalRead;
    result.complete = (totalRead == buffer.size()) || eof();
    return result;
}

IoResult BufferedFileStream::write(std::span<const std::byte> data) {
    IoResult result;
    
    if (!good()) {
        result.error = FileError::IOError;
        return result;
    }
    
    size_t totalWritten = 0;
    size_t remaining = data.size();
    
    while (remaining > 0) {
        // If write buffer is full, flush it
        if (_writePos >= _writeBuffer.size()) {
            flushWriteBuffer();
            if (!good()) {
                result.error = FileError::IOError;
                result.bytesTransferred = totalWritten;
                return result;
            }
        }
        
        // Copy to write buffer
        size_t available = _writeBuffer.size() - _writePos;
        size_t toCopy = std::min(available, remaining);
        
        std::memcpy(_writeBuffer.data() + _writePos,
                   data.data() + totalWritten,
                   toCopy);
        
        _writePos += toCopy;
        totalWritten += toCopy;
        remaining -= toCopy;
        _dirty = true;
    }
    
    result.bytesTransferred = totalWritten;
    result.complete = (totalWritten == data.size());
    return result;
}

bool BufferedFileStream::seek(int64_t offset, std::ios_base::seekdir dir) {
    // Flush write buffer before seeking
    if (_dirty) {
        flushWriteBuffer();
    }
    
    // Invalidate read buffer
    _readPos = 0;
    _readSize = 0;
    
    return _inner->seek(offset, dir);
}

int64_t BufferedFileStream::tell() const {
    // Account for buffered position
    int64_t basePos = _inner->tell();
    if (basePos < 0) return basePos;
    
    // Adjust for buffered reads
    if (_readSize > 0) {
        basePos -= (_readSize - _readPos);
    }
    
    // Adjust for buffered writes
    if (_writePos > 0) {
        basePos += _writePos;
    }
    
    return basePos;
}

bool BufferedFileStream::good() const {
    return _inner->good();
}

bool BufferedFileStream::eof() const {
    return _readPos >= _readSize && _inner->eof();
}

bool BufferedFileStream::fail() const {
    return _inner->fail();
}

void BufferedFileStream::flush() {
    if (_dirty) {
        flushWriteBuffer();
    }
    _inner->flush();
}

void BufferedFileStream::close() {
    if (_dirty) {
        flushWriteBuffer();
    }
    _inner->close();
}

void BufferedFileStream::flushWriteBuffer() {
    if (_writePos > 0) {
        auto result = _inner->write(std::span<const std::byte>(_writeBuffer.data(), _writePos));
        _writePos = 0;
        _dirty = false;
    }
}

void BufferedFileStream::fillReadBuffer() {
    auto result = _inner->read(_readBuffer);
    _readSize = result.bytesTransferred;
    _readPos = 0;
}

} // namespace EntropyEngine::Core::IO