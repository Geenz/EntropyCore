#pragma once
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "FileOperationHandle.h"
#include "IFileSystemBackend.h"  // for WriteOptions

namespace EntropyEngine::Core::IO
{

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
class WriteBatch
{
public:
    WriteBatch(VirtualFileSystem* vfs, std::string path);
    ~WriteBatch() = default;

    // Line operations
    /**
     * @brief Overwrite a line at index (0-based)
     * @param lineNumber Line index to write
     * @param content New content (no newline)
     * @return Reference to this batch for chaining
     */
    WriteBatch& writeLine(size_t lineNumber, std::string_view content);
    /**
     * @brief Insert a line at index (shifts existing lines down)
     * @param lineNumber Insert position
     * @param content Line content
     * @return Reference to this batch for chaining
     */
    WriteBatch& insertLine(size_t lineNumber, std::string_view content);
    /**
     * @brief Delete a line at index (shifts remaining lines up)
     * @param lineNumber Line to delete
     * @return Reference to this batch for chaining
     */
    WriteBatch& deleteLine(size_t lineNumber);
    /**
     * @brief Append a line to the end of the file
     * @param content Line content (no newline)
     * @return Reference to this batch for chaining
     */
    WriteBatch& appendLine(std::string_view content);

    // Bulk operations
    /**
     * @brief Overwrite multiple specific lines in one call
     * @param lines Map of line index -> content; sparse indices allowed
     * @return Reference to this batch for chaining
     */
    WriteBatch& writeLines(const std::map<size_t, std::string>& lines);
    /**
     * @brief Replace entire file content with the provided text
     * @param content New content (may contain multiple lines)
     * @return Reference to this batch for chaining
     */
    WriteBatch& replaceAll(std::string_view content);
    /**
     * @brief Clear the file (equivalent to truncate to zero)
     * @return Reference to this batch for chaining
     */
    WriteBatch& clear();

    // Range operations
    /**
     * @brief Delete a range of lines [startLine, endLine)
     * @param startLine First line to delete (inclusive)
     * @param endLine One past the last line to delete (exclusive)
     * @return Reference to this batch for chaining
     */
    WriteBatch& deleteRange(size_t startLine, size_t endLine);
    /**
     * @brief Insert multiple lines starting at position
     * @param startLine First index at which to insert
     * @param lines Lines to insert
     * @return Reference to this batch for chaining
     */
    WriteBatch& insertLines(size_t startLine, const std::vector<std::string>& lines);

    // Commit changes
    /**
     * @brief Apply all pending operations atomically
     *
     * Uses VFS defaults for parent directory creation and preserves the original
     * file's line-ending style and trailing-newline policy when possible.
     * @return Handle for the asynchronous commit
     * @code
     * auto batch = vfs.createWriteBatch("file.txt");
     * batch->writeLine(0, "Hello").appendLine("World");
     * auto h = batch->commit(); h.wait();
     * @endcode
     */
    FileOperationHandle commit();

    /**
     * @brief Apply all pending operations atomically with per-commit options
     *
     * @param opts WriteOptions controlling behavior:
     *  - createParentDirs: per-commit override for creating parent directories
     *  - ensureFinalNewline: force presence/absence of a final newline on whole-file rewrites
     * @note Line endings are detected from the original file and preserved (LF vs CRLF),
     *       avoiding mixed endings. See VirtualFileSystemExample.cpp for usage.
     * @return Handle for the asynchronous commit
     */
    FileOperationHandle commit(const WriteOptions& opts);

    /**
     * @brief Build the resulting content without writing it (debugging aid)
     * @return Handle whose contentsText() contains the preview after wait()
     */
    FileOperationHandle preview() const;  // Get what the file would look like (for debugging)

    // Query
    /**
     * @brief Number of pending operations in the batch
     */
    size_t pendingOperations() const {
        return _operations.size();
    }
    /**
     * @brief Returns true if no operations are pending
     */
    bool empty() const {
        return _operations.empty();
    }
    /**
     * @brief Clears all pending operations without writing
     */
    void reset();  // Clear all pending operations

    /**
     * @brief Target file path for this batch
     */
    const std::string& getPath() const {
        return _path;
    }

private:
    enum class OpType
    {
        Write,   // Replace line at index
        Insert,  // Insert line, shifting others down
        Delete,  // Delete line, shifting others up
        Append,  // Add to end of file
        Clear,   // Clear entire file
        Replace  // Replace entire file content
    };

    struct Operation
    {
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

}  // namespace EntropyEngine::Core::IO
