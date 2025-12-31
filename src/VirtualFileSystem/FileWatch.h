/**
 * @file FileWatch.h
 * @brief File system watch object using EntropyObject handle facilities
 */
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../Core/EntropyObject.h"

// Forward declare efsw types
namespace efsw
{
using WatchID = long;
}

namespace EntropyEngine::Core::IO
{

// Forward declarations
class FileWatchManager;

/**
 * @brief Types of file system events
 */
enum class FileWatchEvent
{
    Created,   ///< New file or directory created
    Modified,  ///< File content or directory structure changed
    Deleted,   ///< File or directory deleted
    Renamed    ///< File or directory renamed/moved
};

/**
 * @brief Information about a file system event
 */
struct FileWatchInfo
{
    std::string path;                                 ///< Path of the file/directory that changed
    FileWatchEvent event;                             ///< Type of event that occurred
    std::optional<std::string> oldPath;               ///< Previous path (for Renamed events)
    std::chrono::system_clock::time_point timestamp;  ///< When the event occurred
};

/**
 * @brief Callback function for file system events
 */
using FileWatchCallback = std::function<void(const FileWatchInfo&)>;

/**
 * @brief Options for configuring file system watches
 */
struct WatchOptions
{
    bool recursive = true;                     ///< Watch subdirectories recursively
    bool followSymlinks = false;               ///< Follow symbolic links when watching
    std::vector<std::string> includePatterns;  ///< Include only files matching these patterns (*.cpp, *.h)
    std::vector<std::string> excludePatterns;  ///< Exclude files matching these patterns (.git/*, *.tmp)
};

/**
 * @brief File system watch object (refcounted, handle-stampable)
 *
 * FileWatch is an EntropyObject that represents an active file system watch.
 * The object is refcounted and can be stamped with handle identity by the
 * FileWatchManager for validation. When the last reference is released,
 * the destructor automatically stops the watch.
 *
 * Usage:
 * @code
 * FileWatch* watch = vfs.watchDirectory("./src", callback, opts);
 * // watch has refcount=1, you own the reference
 *
 * // Later, stop and release
 * watch->stop();      // Stop watching
 * watch->release();   // Decrement refcount (may delete if 0)
 * @endcode
 */
class FileWatch : public EntropyObject
{
public:
    /**
     * @brief Constructs a file watch (internal - use FileWatchManager::createWatch)
     */
    FileWatch(FileWatchManager* owner, const std::string& path, FileWatchCallback callback,
              const WatchOptions& options);

    ~FileWatch() noexcept override;

    /**
     * @brief Stops watching the file system
     * @note Idempotent - safe to call multiple times
     */
    void stop();

    /**
     * @brief Checks if this watch is currently active
     */
    bool isWatching() const noexcept {
        return _watching.load(std::memory_order_acquire);
    }

    /**
     * @brief Gets the path being watched
     */
    const std::string& path() const noexcept {
        return _path;
    }

    /**
     * @brief Gets the watch options
     */
    const WatchOptions& options() const noexcept {
        return _options;
    }

    // EntropyObject overrides
    const char* className() const noexcept override {
        return "FileWatch";
    }

private:
    FileWatchManager* _owner;            ///< Owning manager
    std::string _path;                   ///< Watched path
    FileWatchCallback _callback;         ///< User callback
    WatchOptions _options;               ///< Watch configuration
    efsw::WatchID _efswId = 0;           ///< efsw watch ID (0 = invalid)
    std::atomic<bool> _watching{false};  ///< true if actively watching

    friend class FileWatchManager;
};

}  // namespace EntropyEngine::Core::IO
