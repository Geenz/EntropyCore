/**
 * @file FileWatchManager.cpp
 * @brief Implementation of FileWatchManager using EntropyObject handle stamping
 */
#include "FileWatchManager.h"

#include <algorithm>
#include <efsw/efsw.hpp>

#include "../Logging/Logger.h"
#include "VirtualFileSystem.h"

namespace EntropyEngine::Core::IO
{

// Simple glob matching helper
static bool matchGlob(const std::string& str, const std::string& pattern) {
    size_t strIdx = 0;
    size_t patIdx = 0;
    size_t starIdx = std::string::npos;
    size_t matchIdx = 0;

    while (strIdx < str.size()) {
        if (patIdx < pattern.size() && (pattern[patIdx] == '?' || pattern[patIdx] == str[strIdx])) {
            ++strIdx;
            ++patIdx;
        } else if (patIdx < pattern.size() && pattern[patIdx] == '*') {
            starIdx = patIdx++;
            matchIdx = strIdx;
        } else if (starIdx != std::string::npos) {
            patIdx = starIdx + 1;
            strIdx = ++matchIdx;
        } else {
            return false;
        }
    }

    while (patIdx < pattern.size() && pattern[patIdx] == '*') {
        ++patIdx;
    }

    return patIdx == pattern.size();
}

/**
 * @brief efsw listener implementation that dispatches to FileWatchManager
 */
class FileWatchListener : public efsw::FileWatchListener
{
public:
    explicit FileWatchListener(FileWatchManager* manager) : _manager(manager) {}

    void handleFileAction(efsw::WatchID watchId, const std::string& dir, const std::string& filename,
                          efsw::Action action, std::string oldFilename) override {
        // Find which slot this watch belongs to
        uint32_t slotIndex = _manager->findSlotByEfswId(watchId);
        if (slotIndex == UINT32_MAX) {
            return;  // Watch no longer exists
        }

        // Convert efsw action to our event type
        FileWatchEvent event;
        switch (action) {
            case efsw::Actions::Add:
                event = FileWatchEvent::Created;
                break;
            case efsw::Actions::Delete:
                event = FileWatchEvent::Deleted;
                break;
            case efsw::Actions::Modified:
                event = FileWatchEvent::Modified;
                break;
            case efsw::Actions::Moved:
                event = FileWatchEvent::Renamed;
                break;
            default:
                return;  // Unknown action
        }

        // Build full path
        std::string fullPath = dir;
        if (!fullPath.empty() && fullPath.back() != '/' && fullPath.back() != '\\') {
            fullPath += '/';
        }
        fullPath += filename;

        // Build event info
        FileWatchInfo info;
        info.path = fullPath;
        info.event = event;
        info.timestamp = std::chrono::system_clock::now();

        if (event == FileWatchEvent::Renamed && !oldFilename.empty()) {
            std::string oldFullPath = dir;
            if (!oldFullPath.empty() && oldFullPath.back() != '/' && oldFullPath.back() != '\\') {
                oldFullPath += '/';
            }
            oldFullPath += oldFilename;
            info.oldPath = oldFullPath;
        }

        // Dispatch to manager
        _manager->onFileEvent(slotIndex, info);
    }

private:
    FileWatchManager* _manager;
};

// FileWatchManager implementation

FileWatchManager::FileWatchManager(VirtualFileSystem* vfs)
    : _vfs(vfs), _listener(std::make_unique<FileWatchListener>(this)) {}

FileWatchManager::~FileWatchManager() {
    // Phase 1: Stop all watches and collect them (holding _slotMutex)
    std::vector<FileWatch*> watchesToRelease;
    {
        std::lock_guard lock(_slotMutex);

        for (auto& slot : _slots) {
            if (slot.occupied && slot.watch) {
                slot.watch->stop();
                watchesToRelease.push_back(slot.watch);
                slot.watch = nullptr;
            }
        }
    }
    // _slotMutex is now unlocked

    // Phase 2: Destroy watcher and listener (NOT holding _slotMutex to avoid lock-order-inversion)
    // This may trigger efsw to lock its internal mutex
    _watcher.reset();
    _listener.reset();

    // Phase 3: Release watch references (no locks needed, refcount is atomic)
    for (FileWatch* watch : watchesToRelease) {
        watch->release();  // Release the manager's reference
    }

    // Phase 4: Clear the ID map
    {
        std::lock_guard mapLock(_efswIdMapMutex);
        _efswIdToSlot.clear();
    }
}

FileWatch* FileWatchManager::createWatch(const std::string& path, FileWatchCallback callback,
                                         const WatchOptions& options) {
    uint32_t index;
    uint32_t generation;
    FileWatch* watch = nullptr;
    efsw::FileWatcher* watcher = nullptr;

    // Phase 1: Allocate slot and create watch (holding _slotMutex)
    {
        std::lock_guard lock(_slotMutex);

        // Ensure efsw watcher is initialized
        ensureWatcherInitialized();
        if (!_watcher) {
            ENTROPY_LOG_ERROR("Failed to initialize file watcher");
            return nullptr;
        }

        // Allocate slot
        auto slot_info = allocateSlot();
        index = slot_info.first;
        generation = slot_info.second;
        if (index == UINT32_MAX) {
            ENTROPY_LOG_ERROR("Failed to allocate watch slot (out of slots)");
            return nullptr;
        }

        // Create FileWatch object (refcount starts at 1)
        watch = new FileWatch(this, path, callback, options);

        // Stamp the object with handle identity using EntropyObject's built-in facility
        HandleAccess::set(*watch, this, index, generation);

        // Store in slot immediately so it's discoverable
        WatchSlot& slot = _slots[index];
        slot.watch = watch;
        slot.generation = generation;
        slot.occupied = true;

        // Cache watcher pointer for use outside the lock
        watcher = _watcher.get();
    }
    // _slotMutex is now unlocked

    // Phase 2: Add watch to efsw (NOT holding _slotMutex to avoid lock-order-inversion)
    // efsw may lock its internal mutex here
    efsw::WatchID efswId = watcher->addWatch(path, _listener.get(), options.recursive);

    if (efswId < 0) {
        // Failed - clean up the slot
        ENTROPY_LOG_ERROR("Failed to add watch for path: " + path);

        std::lock_guard lock(_slotMutex);
        freeSlot(index);
        HandleAccess::clear(*watch);
        watch->release();  // Delete the watch
        return nullptr;
    }

    // Phase 3: Store efswId in the watch and in the ID map
    watch->_efswId = efswId;
    watch->_watching.store(true, std::memory_order_release);

    {
        std::lock_guard lock(_efswIdMapMutex);
        _efswIdToSlot[efswId] = index;
    }

    ENTROPY_LOG_INFO("Created file watch for: " + path + " (slot " + std::to_string(index) + ", efswId " +
                     std::to_string(efswId) + ")");

    // Return with refcount=1 (caller owns the reference)
    return watch;
}

void FileWatchManager::destroyWatch(FileWatch* watch) {
    if (!watch) {
        return;
    }

    watch->stop();
    watch->release();  // Decrement refcount (may delete)
}

bool FileWatchManager::isValid(const FileWatch* watch) const {
    if (!watch || !watch->hasHandle()) {
        return false;
    }

    // Check that watch is owned by this manager
    if (watch->handleOwnerAs<FileWatchManager>() != this) {
        return false;
    }

    std::lock_guard lock(_slotMutex);

    uint32_t index = watch->handleIndex();
    uint32_t generation = watch->handleGeneration();

    return index < _slots.size() && _slots[index].occupied && _slots[index].generation == generation &&
           _slots[index].watch == watch;
}

std::pair<uint32_t, uint32_t> FileWatchManager::allocateSlot() {
    // Find free slot
    for (size_t i = 0; i < _slots.size(); ++i) {
        if (!_slots[i].occupied) {
            _slots[i].occupied = true;
            return {static_cast<uint32_t>(i), _slots[i].generation};
        }
    }

    // No free slots, allocate new one
    uint32_t index = static_cast<uint32_t>(_slots.size());
    _slots.push_back(WatchSlot{});
    _slots.back().occupied = true;
    _slots.back().generation = 0;

    return {index, 0};
}

void FileWatchManager::freeSlot(uint32_t index) {
    if (index >= _slots.size()) {
        return;
    }

    WatchSlot& slot = _slots[index];
    slot.occupied = false;
    slot.watch = nullptr;
    slot.generation++;  // Increment generation to invalidate existing handles
}

void FileWatchManager::ensureWatcherInitialized() {
    if (_watcher) {
        return;  // Already initialized
    }

    _watcher = std::make_unique<efsw::FileWatcher>();
    _watcher->watch();  // Start watching
}

bool FileWatchManager::matchesFilters(const std::string& path, const WatchOptions& options) const {
    // Check exclude patterns first
    for (const auto& pattern : options.excludePatterns) {
        if (matchGlob(path, pattern)) {
            return false;  // Excluded
        }
    }

    // If no include patterns, accept all (not excluded)
    if (options.includePatterns.empty()) {
        return true;
    }

    // Check include patterns
    for (const auto& pattern : options.includePatterns) {
        if (matchGlob(path, pattern)) {
            return true;  // Included
        }
    }

    return false;  // Not in include list
}

void FileWatchManager::onFileEvent(uint32_t slotIndex, const FileWatchInfo& info) {
    // Called from efsw thread - need to lock and validate
    std::unique_lock lock(_slotMutex);

    if (slotIndex >= _slots.size() || !_slots[slotIndex].occupied) {
        return;  // Slot no longer valid
    }

    WatchSlot& slot = _slots[slotIndex];
    FileWatch* watch = slot.watch;

    if (!watch || !watch->isWatching()) {
        return;  // Watch stopped
    }

    // Check filters
    if (!matchesFilters(info.path, watch->options())) {
        return;  // Filtered out
    }

    // Copy callback (so we can release lock before invoking)
    FileWatchCallback callback = watch->_callback;
    lock.unlock();

    // Dispatch to WorkContractGroup via VFS for thread safety
    if (_vfs && callback) {
        _vfs->submit(info.path,
                     [callback, info](FileOperationHandle::OpState& s, const std::string&, const ExecContext&) {
                         callback(info);
                         s.complete(FileOpStatus::Complete);
                     });
    }
}

uint32_t FileWatchManager::findSlotByEfswId(efsw::WatchID efswId) const {
    // Use the separate ID map to avoid lock-order-inversion with efsw's internal mutex
    std::lock_guard lock(_efswIdMapMutex);

    auto it = _efswIdToSlot.find(efswId);
    if (it != _efswIdToSlot.end()) {
        return it->second;
    }

    return UINT32_MAX;  // Not found
}

void FileWatchManager::removeEfswWatch(FileWatch* watch) {
    if (!watch) {
        return;
    }

    efsw::WatchID efswId = 0;
    efsw::FileWatcher* watcher = nullptr;
    uint32_t index = UINT32_MAX;

    // Phase 1: Get efswId and free the slot (holding _slotMutex)
    {
        std::lock_guard lock(_slotMutex);

        efswId = watch->_efswId;
        watcher = _watcher.get();

        // Free slot if this watch is still in it
        if (watch->hasHandle() && watch->handleOwnerAs<FileWatchManager>() == this) {
            index = watch->handleIndex();
            if (index < _slots.size() && _slots[index].watch == watch) {
                freeSlot(index);
                HandleAccess::clear(*watch);
            }
        }
    }
    // _slotMutex is now unlocked

    // Phase 2: Remove from efsw (NOT holding _slotMutex to avoid lock-order-inversion)
    if (watcher && efswId != 0) {
        watcher->removeWatch(efswId);
        watch->_efswId = 0;
    }

    // Phase 3: Remove from ID map
    if (efswId != 0) {
        std::lock_guard lock(_efswIdMapMutex);
        _efswIdToSlot.erase(efswId);
    }
}

}  // namespace EntropyEngine::Core::IO
