#include "WriteBatch.h"
#include "VirtualFileSystem.h"
#include "FileHandle.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <random>
#include <map>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace EntropyEngine::Core::IO {

WriteBatch::WriteBatch(VirtualFileSystem* vfs, std::string path) 
    : _vfs(vfs), _path(std::move(path)) {
}

WriteBatch& WriteBatch::writeLine(size_t lineNumber, std::string_view content) {
    _operations.push_back({OpType::Write, lineNumber, std::string(content)});
    return *this;
}

WriteBatch& WriteBatch::insertLine(size_t lineNumber, std::string_view content) {
    _operations.push_back({OpType::Insert, lineNumber, std::string(content)});
    return *this;
}

WriteBatch& WriteBatch::deleteLine(size_t lineNumber) {
    _operations.push_back({OpType::Delete, lineNumber, ""});
    return *this;
}

WriteBatch& WriteBatch::appendLine(std::string_view content) {
    _operations.push_back({OpType::Append, 0, std::string(content)});
    return *this;
}

WriteBatch& WriteBatch::writeLines(const std::map<size_t, std::string>& lines) {
    for (const auto& [lineNum, content] : lines) {
        writeLine(lineNum, content);
    }
    return *this;
}

WriteBatch& WriteBatch::replaceAll(std::string_view content) {
    _operations.clear();
    _operations.push_back({OpType::Replace, 0, std::string(content)});
    return *this;
}

WriteBatch& WriteBatch::clear() {
    _operations.clear();
    _operations.push_back({OpType::Clear, 0, ""});
    return *this;
}

WriteBatch& WriteBatch::deleteRange(size_t startLine, size_t endLine) {
    for (size_t i = startLine; i <= endLine; ++i) {
        deleteLine(i);
    }
    return *this;
}

WriteBatch& WriteBatch::insertLines(size_t startLine, const std::vector<std::string>& lines) {
    size_t offset = 0;
    for (const auto& line : lines) {
        insertLine(startLine + offset, line);
        offset++;
    }
    return *this;
}

void WriteBatch::reset() {
    _operations.clear();
}

std::vector<std::string> WriteBatch::applyOperations(const std::vector<std::string>& originalLines) const {
    std::vector<std::string> result = originalLines;

    // Separate operations by type for proper ordering
    std::vector<Operation> replaceOps;
    std::map<size_t, std::string> writeOps;  // Use map to handle sparse writes
    std::vector<Operation> insertOps;
    std::vector<Operation> deleteOps;
    std::vector<std::string> appendOps;
    bool shouldClear = false;

    for (const auto& op : _operations) {
        switch (op.type) {
            case OpType::Clear:
                shouldClear = true;
                break;
            case OpType::Replace:
                replaceOps.push_back(op);
                break;
            case OpType::Write:
                writeOps[op.lineNumber] = op.content;  // Later writes override earlier ones
                break;
            case OpType::Insert:
                insertOps.push_back(op);
                break;
            case OpType::Delete:
                deleteOps.push_back(op);
                break;
            case OpType::Append:
                appendOps.push_back(op.content);
                break;
        }
    }

    // Process Clear
    if (shouldClear) {
        result.clear();
    }

    // Process Replace operations (they clear the file)
    for (const auto& op : replaceOps) {
        result.clear();
        std::istringstream iss(op.content);
        std::string line;
        while (std::getline(iss, line)) {
            result.push_back(line);
        }
    }

    // Process Delete operations (highest index first to avoid shifting issues)
    std::sort(deleteOps.begin(), deleteOps.end(),
        [](const Operation& a, const Operation& b) { return a.lineNumber > b.lineNumber; });
    for (const auto& op : deleteOps) {
        if (op.lineNumber < result.size()) {
            result.erase(result.begin() + op.lineNumber);
        }
    }

    // Process Insert operations BEFORE writes (highest index first to avoid shifting issues)
    std::sort(insertOps.begin(), insertOps.end(),
        [](const Operation& a, const Operation& b) { return a.lineNumber > b.lineNumber; });
    for (const auto& op : insertOps) {
        if (op.lineNumber > result.size()) {
            result.resize(op.lineNumber);
        }
        result.insert(result.begin() + op.lineNumber, op.content);
    }

    // Process Write operations - preserve sparse line numbers
    if (!writeOps.empty()) {
        size_t maxLine = writeOps.rbegin()->first;
        if (maxLine >= result.size()) {
            result.resize(maxLine + 1);
        }
        for (const auto& [lineNum, content] : writeOps) {
            result[lineNum] = content;
        }
    }

    // Process Append operations last
    for (const auto& content : appendOps) {
        result.push_back(content);
    }

    return result;
}

FileOperationHandle WriteBatch::commit() {
    if (_operations.empty()) {
        // No operations to commit - return immediate success
        return FileOperationHandle::immediate(FileOpStatus::Complete);
    }
    
    auto lock = _vfs->lockForPath(_path);
    
    return _vfs->submit(_path, [this, lock, ops = _operations](FileOperationHandle::OpState& s, const std::string& p) mutable {
        std::unique_lock<std::mutex> pathLock;
        if (lock) pathLock = std::unique_lock<std::mutex>(*lock);
        
        std::error_code ec;
        
        // Read the original file
        std::vector<std::string> originalLines;
        {
            std::ifstream in(p, std::ios::in);
            if (in) {
                std::string line;
                while (std::getline(in, line)) {
                    originalLines.push_back(line);
                }
            }
        }
        
        // Apply all operations to get final content
        std::vector<std::string> finalLines = applyOperations(originalLines);
        
        // Generate temporary filename
        auto targetPath = std::filesystem::path(p);
        auto dir = targetPath.parent_path();
        auto base = targetPath.filename().string();
        auto tempPath = dir / (base + ".tmp" + std::to_string(std::random_device{}()));
        
        // Ensure parent directory exists if configured by VFS default
        if (_vfs && _vfs->_cfg.defaultCreateParentDirs) {
            std::error_code dirEc;
            if (!dir.empty()) {
                std::filesystem::create_directories(dir, dirEc);
                if (dirEc) {
                    s.setError(FileError::IOError, "Failed to create parent directories", dir.string(), dirEc);
                    s.complete(FileOpStatus::Failed);
                    return;
                }
            }
        }
        
        // Write to temp file
        {
            std::ofstream out(tempPath, std::ios::out | std::ios::trunc);
            if (!out) {
                s.setError(FileError::IOError, "Failed to create temp file", tempPath.string());
                s.complete(FileOpStatus::Failed);
                return;
            }
            
            for (size_t i = 0; i < finalLines.size(); ++i) {
                out << finalLines[i];
                if (i < finalLines.size() - 1) {
                    out << "\n";
                }
            }
            
            // Add final newline if file isn't empty
            if (!finalLines.empty()) {
                out << "\n";
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
        #if defined(_WIN32)
            // Use Windows-specific atomic rename with retry logic
            auto wsrc = tempPath.wstring();
            auto wdst = targetPath.wstring();
            
            const int maxRetries = 50;
            const int retryDelayMs = 10;
            
            bool success = false;
            for (int i = 0; i < maxRetries; ++i) {
                if (MoveFileExW(wsrc.c_str(), wdst.c_str(), 
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
                    success = true;
                    break;
                }
                
                DWORD error = GetLastError();
                if (error == ERROR_SHARING_VIOLATION || 
                    error == ERROR_ACCESS_DENIED || 
                    error == ERROR_LOCK_VIOLATION) {
                    Sleep(retryDelayMs);
                    continue;
                }
                break;
            }
            
            if (!success) {
                std::filesystem::remove(tempPath, ec);
                s.setError(FileError::IOError, "Failed to replace destination file", p);
                s.complete(FileOpStatus::Failed);
                return;
            }
        #else
            std::filesystem::rename(tempPath, targetPath, ec);
            if (ec) {
                std::filesystem::remove(tempPath, ec);
                s.setError(FileError::IOError, "Failed to rename temp file", p, ec);
                s.complete(FileOpStatus::Failed);
                return;
            }
        #endif
        
        // Calculate total bytes written
        size_t totalBytes = 0;
        for (const auto& line : finalLines) {
            totalBytes += line.size() + 1; // +1 for newline
        }
        
        s.wrote = totalBytes;
        s.complete(FileOpStatus::Complete);
    });
}

FileOperationHandle WriteBatch::preview() const {
    return _vfs->submit(_path, [this, ops = _operations](FileOperationHandle::OpState& s, const std::string& p) {
        // Read the original file
        std::vector<std::string> originalLines;
        {
            std::ifstream in(p, std::ios::in);
            if (in) {
                std::string line;
                while (std::getline(in, line)) {
                    originalLines.push_back(line);
                }
            }
        }
        
        // Apply all operations to get final content
        std::vector<std::string> finalLines = applyOperations(originalLines);
        
        // Build result string
        std::ostringstream oss;
        for (size_t i = 0; i < finalLines.size(); ++i) {
            oss << finalLines[i];
            if (i < finalLines.size() - 1) {
                oss << "\n";
            }
        }
        
        s.text = oss.str();
        s.complete(FileOpStatus::Complete);
    });
}

} // namespace EntropyEngine::Core::IO