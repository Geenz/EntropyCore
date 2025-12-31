/**
 * @file FileWatch.cpp
 * @brief Implementation of FileWatch EntropyObject
 */
#include "FileWatch.h"

#include "../Logging/Logger.h"
#include "FileWatchManager.h"

namespace EntropyEngine::Core::IO
{

FileWatch::FileWatch(FileWatchManager* owner, const std::string& path, FileWatchCallback callback,
                     const WatchOptions& options)
    : _owner(owner), _path(path), _callback(std::move(callback)), _options(options), _efswId(0), _watching(false) {}

FileWatch::~FileWatch() noexcept {
    // Ensure watch is stopped before destruction
    stop();
}

void FileWatch::stop() {
    bool wasWatching = _watching.exchange(false, std::memory_order_acq_rel);
    if (!wasWatching) {
        return;  // Already stopped
    }

    // Remove from efsw through manager
    if (_owner) {
        _owner->removeEfswWatch(this);
    }

    ENTROPY_LOG_INFO("Stopped file watch for: " + _path);
}

}  // namespace EntropyEngine::Core::IO
