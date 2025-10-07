/**
 * @file FileWatchManager.h
 * @brief Manager for FileWatch objects with handle stamping and slot-based storage
 */
#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include "FileWatch.h"

// Forward declare efsw types
namespace efsw {
    class FileWatcher;
    class FileWatchListener;
}

namespace EntropyEngine::Core::IO {

// Forward declarations
class VirtualFileSystem;

/**
 * @brief Manager for FileWatch objects using EntropyObject handle stamping
 *
 * FileWatchManager owns the efsw::FileWatcher instance and manages FileWatch
 * objects in slots. When creating a watch, the manager allocates a slot,
 * creates the FileWatch object, and stamps it with handle identity using
 * HandleAccess::set(). This enables generation-based validation.
 *
 * Thread safety: All public methods are thread-safe via mutex.
 */
class FileWatchManager {
public:
    explicit FileWatchManager(VirtualFileSystem* vfs);
    ~FileWatchManager();

    // Non-copyable, non-movable
    FileWatchManager(const FileWatchManager&) = delete;
    FileWatchManager& operator=(const FileWatchManager&) = delete;
    FileWatchManager(FileWatchManager&&) = delete;
    FileWatchManager& operator=(FileWatchManager&&) = delete;

    /**
     * @brief Creates a new file system watch
     * @param path Directory or file path to watch
     * @param callback Function to call when events occur
     * @param options Watch configuration options
     * @return FileWatch* with refcount=1 (caller owns the reference), or nullptr if failed
     */
    FileWatch* createWatch(const std::string& path,
                          FileWatchCallback callback,
                          const WatchOptions& options);

    /**
     * @brief Stops a watch and releases the reference
     * @param watch Watch to stop (may be nullptr)
     * @note Calls watch->stop() and watch->release()
     */
    void destroyWatch(FileWatch* watch);

    /**
     * @brief Validates a FileWatch object using handle stamping
     * @param watch Watch to validate
     * @return true if watch is valid and still active
     */
    bool isValid(const FileWatch* watch) const;

private:
    /**
     * @brief Storage slot for a FileWatch
     */
    struct WatchSlot {
        FileWatch* watch = nullptr;     ///< Pointer to the watch object (or nullptr if free)
        uint32_t generation = 0;        ///< Generation counter for validation
        bool occupied = false;          ///< true if slot is in use
    };

    VirtualFileSystem* _vfs;                              ///< Parent VFS (for thread dispatch)
    std::unique_ptr<efsw::FileWatcher> _watcher;          ///< efsw file watcher instance (lazy-initialized)
    std::unique_ptr<efsw::FileWatchListener> _listener;   ///< efsw event listener
    std::vector<WatchSlot> _slots;                        ///< Slot-based storage
    mutable std::mutex _slotMutex;                        ///< Protects slots and watcher

    /**
     * @brief Allocates a new slot for a watch
     * @return Pair of (slot index, generation) or (UINT32_MAX, 0) if failed
     */
    std::pair<uint32_t, uint32_t> allocateSlot();

    /**
     * @brief Frees a slot, incrementing its generation
     * @param index Slot index to free
     */
    void freeSlot(uint32_t index);

    /**
     * @brief Initializes efsw watcher if not already created
     */
    void ensureWatcherInitialized();

    /**
     * @brief Checks if a path matches include/exclude patterns
     */
    bool matchesFilters(const std::string& path, const WatchOptions& options) const;

    /**
     * @brief Internal callback from efsw (executed on efsw thread)
     */
    void onFileEvent(uint32_t slotIndex, const FileWatchInfo& info);

    /**
     * @brief Finds slot index by efsw watch ID
     */
    uint32_t findSlotByEfswId(efsw::WatchID efswId) const;

    /**
     * @brief Removes efsw watch for a FileWatch object
     */
    void removeEfswWatch(FileWatch* watch);

    friend class FileWatchListener; // efsw listener needs access
    friend class FileWatch;         // FileWatch needs access to removeEfswWatch
};

} // namespace EntropyEngine::Core::IO
