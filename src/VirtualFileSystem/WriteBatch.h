#pragma once
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <variant>
#include "FileOperationHandle.h"
#include "IFileSystemBackend.h" // for WriteOptions

namespace EntropyEngine::Core::IO {

class VirtualFileSystem;
class FileHandle;

/**
 * WriteBatch - Collects multiple write operations and applies them atomically
 * 
 * This allows for efficient batch processing of file modifications without
 * repeatedly reading and writing the entire file. All operations are collected
 * in memory and then applied in a single atomic operation.
 * 
 * Usage:
 *   auto batch = vfs->createWriteBatch("myfile.txt");
 *   batch->writeLine(0, "First line");
 *   batch->writeLine(5, "Sixth line");  
 *   batch->insertLine(2, "New third line");
 *   batch->deleteLine(10);
 *   auto handle = batch->commit();  // Applies all changes atomically
 *   handle.wait();
 */
class WriteBatch {
public:
    WriteBatch(VirtualFileSystem* vfs, std::string path);
    ~WriteBatch() = default;
    
    // Line operations
    WriteBatch& writeLine(size_t lineNumber, std::string_view content);
    WriteBatch& insertLine(size_t lineNumber, std::string_view content);
    WriteBatch& deleteLine(size_t lineNumber);
    WriteBatch& appendLine(std::string_view content);
    
    // Bulk operations
    WriteBatch& writeLines(const std::map<size_t, std::string>& lines);
    WriteBatch& replaceAll(std::string_view content);
    WriteBatch& clear();
    
    // Range operations
    WriteBatch& deleteRange(size_t startLine, size_t endLine);
    WriteBatch& insertLines(size_t startLine, const std::vector<std::string>& lines);
    
    // Commit changes
    FileOperationHandle commit();        // Apply all changes atomically (uses defaults)
    FileOperationHandle commit(const WriteOptions& opts); // Apply changes with per-commit options
    FileOperationHandle preview() const; // Get what the file would look like (for debugging)
    
    // Query
    size_t pendingOperations() const { return _operations.size(); }
    bool empty() const { return _operations.empty(); }
    void reset(); // Clear all pending operations
    
    const std::string& getPath() const { return _path; }
    
private:
    enum class OpType {
        Write,    // Replace line at index
        Insert,   // Insert line, shifting others down
        Delete,   // Delete line, shifting others up
        Append,   // Add to end of file
        Clear,    // Clear entire file
        Replace   // Replace entire file content
    };
    
    struct Operation {
        OpType type;
        size_t lineNumber;
        std::string content;
    };
    
    VirtualFileSystem* _vfs;
    std::string _path;
    std::vector<Operation> _operations;
    
    // Apply operations to build final file content
    std::vector<std::string> applyOperations(const std::vector<std::string>& originalLines) const;
};

} // namespace EntropyEngine::Core::IO