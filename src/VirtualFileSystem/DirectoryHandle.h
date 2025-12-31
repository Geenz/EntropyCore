/**
 * @file DirectoryHandle.h
 * @brief Value-semantic handle for directory operations through VFS
 *
 * DirectoryHandle provides a convenient, copyable reference to a directory path routed through a
 * VirtualFileSystem backend. Use it to create, remove, and list directories. Like FileHandle,
 * equality and hashing are backend-aware via a normalized key.
 */
#pragma once
#include <memory>
#include <string>

#include "FileOperationHandle.h"
#include "IFileSystemBackend.h"

namespace EntropyEngine::Core::IO
{

class VirtualFileSystem;   // fwd
class IFileSystemBackend;  // fwd

/**
 * @brief Copyable handle to a directory path routed through a backend
 *
 * Construct via VirtualFileSystem::createDirectoryHandle(). Operations are asynchronous;
 * call wait() on the returned FileOperationHandle to block, or chain operations.
 *
 * Design note: DirectoryHandle is a dumb handle; it does not probe the filesystem and
 * contains no backend-specific behavior. All operations delegate to the routed backend
 * through the VirtualFileSystem.
 *
 * @code
 * WorkContractGroup group(2000);
 * VirtualFileSystem vfs(&group);
 * auto dh = vfs.createDirectoryHandle("my_folder");
 * dh.create().wait();
 * auto listing = dh.list(); listing.wait();
 * for (const auto& entry : listing.directoryEntries()) {
 *     ENTROPY_LOG_INFO("Entry: " + entry.name);
 * }
 * dh.remove(true).wait(); // recursive
 * @endcode
 */
class DirectoryHandle
{
public:
    struct Metadata
    {
        std::string path;       // full path as provided
        std::string directory;  // parent directory (may be empty)
        std::string name;       // directory name
        bool exists = false;    // whether directory exists at construction time
    };

private:
    /**
     * @brief Constructs a handle bound to a VirtualFileSystem and path
     *
     * Use VirtualFileSystem::createDirectoryHandle() to obtain instances. The handle is copyable
     * and cheap to pass by value. Operations are asynchronous; call wait() on returned handles.
     * @param vfs VFS instance that will execute operations
     * @param path Target directory path (routed to a backend)
     */
    explicit DirectoryHandle(VirtualFileSystem* vfs, std::string path);
    DirectoryHandle() = delete;

public:
    // Directory operations
    /**
     * @brief Creates the directory at this path
     *
     * Asynchronously creates the directory. Behavior is backend-specific.
     * Note: Currently, parameters are hints and may be ignored by the backend.
     * - LocalFileSystem: Always creates all parent directories (mkdir -p semantics),
     *   regardless of createParents. Operation is idempotent.
     * - Object storage (S3/Azure): May create 0-byte marker object (future).
     * @param createParents Hint to create parent directories as needed (currently ignored)
     * @return Handle representing the create operation
     */
    FileOperationHandle create(bool createParents = true) const;

    /**
     * @brief Removes the directory at this path
     *
     * Asynchronously removes the directory. Behavior is backend-specific:
     * - LocalFileSystem: Fails if non-empty unless recursive=true
     * - Object storage (S3/Azure): Deletes all objects with this prefix (potentially expensive)
     * @param recursive If true, remove contents recursively (default false)
     * @return Handle representing the remove operation
     */
    FileOperationHandle remove(bool recursive = false) const;

    /**
     * @brief Lists the contents of this directory
     *
     * Asynchronously lists directory contents. Call directoryEntries() on the returned
     * handle after wait() to access results.
     * @param options Listing options (recursion, filters, sorting, pagination)
     * @return Handle whose directoryEntries() is populated after wait()
     */
    FileOperationHandle list(const ListDirectoryOptions& options = {}) const;

    /**
     * @brief Retrieves metadata for this directory
     *
     * Asynchronously retrieves directory metadata (exists, permissions, timestamps).
     * @return Handle whose metadata() is populated after wait()
     */
    FileOperationHandle getMetadata() const;

    /**
     * @brief Returns static metadata captured at handle construction
     * @return Reference to directory metadata (existence at creation time, etc.)
     */
    const Metadata& metadata() const noexcept {
        return _meta;
    }

    /**
     * @brief Backend-aware normalized key for identity/locking
     * @return Normalized key string used for equality
     */
    const std::string& normalizedKey() const noexcept {
        return _normKey;
    }

    // Equality based on backend identity and normalized key
    friend bool operator==(const DirectoryHandle& a, const DirectoryHandle& b) noexcept {
        return a._backend.get() == b._backend.get() && a._normKey == b._normKey;
    }
    friend bool operator!=(const DirectoryHandle& a, const DirectoryHandle& b) noexcept {
        return !(a == b);
    }

private:
    VirtualFileSystem* _vfs;
    std::shared_ptr<IFileSystemBackend> _backend;  // Backend for this directory (ref-counted for safety)
    Metadata _meta;                                // associated metadata for this handle
    std::string _normKey;                          // backend-normalized key captured at creation

    friend class VirtualFileSystem;
    // Allow hasher to access private members without exposing opaque pointers publicly
    friend struct std::hash<EntropyEngine::Core::IO::DirectoryHandle>;
};

}  // namespace EntropyEngine::Core::IO

// Hash support for DirectoryHandle
namespace std
{
template <>
struct hash<EntropyEngine::Core::IO::DirectoryHandle>
{
    size_t operator()(const EntropyEngine::Core::IO::DirectoryHandle& dh) const noexcept {
        // Combine backend identity pointer and normalized key
        size_t h1 = hash<const void*>()(dh._backend.get());
        size_t h2 = hash<string>()(dh.normalizedKey());
        return h1 ^ (h2 << 1);
    }
};
}  // namespace std
