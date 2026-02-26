/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "MemoryTracker.h"

#include <algorithm>
#include <format>
#include <vector>

#include "../Logging/Logger.h"
#include "EntropyObject.h"

namespace EntropyEngine::Core
{

MemoryTracker& MemoryTracker::instance() {
    static MemoryTracker sInstance;
    return sInstance;
}

void MemoryTracker::install() {
    if (_installed) return;

    // Capture any existing hooks (e.g., Tracy) to chain after our tracking
    _chainedAlloc = EntropyObjectMemoryHooks::onAlloc;
    _chainedFree = EntropyObjectMemoryHooks::onFree;

    EntropyObjectMemoryHooks::onAlloc = [](void* ptr, size_t size, const char* className) {
        auto& tracker = MemoryTracker::instance();
        tracker.onAlloc(ptr, size, className);
        if (tracker._chainedAlloc) {
            tracker._chainedAlloc(ptr, size, className);
        }
    };

    EntropyObjectMemoryHooks::onFree = [](void* ptr, const char* className) {
        auto& tracker = MemoryTracker::instance();
        tracker.onFree(ptr, className);
        if (tracker._chainedFree) {
            tracker._chainedFree(ptr, className);
        }
    };

    _installed = true;
    ENTROPY_LOG_INFO(std::format("MemoryTracker: Installed (warning threshold: {} allocs/frame)", _warningThreshold));
}

void MemoryTracker::onAlloc(void* /*ptr*/, size_t size, const char* className) {
    ClassMemoryStats* stats = getOrCreateStats(className);
    stats->liveCount.fetch_add(1, std::memory_order_relaxed);
    stats->liveBytes.fetch_add(static_cast<int64_t>(size), std::memory_order_relaxed);
    stats->totalAllocs.fetch_add(1, std::memory_order_relaxed);
    stats->frameAllocs.fetch_add(1, std::memory_order_relaxed);
    stats->frameAllocBytes.fetch_add(static_cast<int64_t>(size), std::memory_order_relaxed);
}

void MemoryTracker::onFree(void* /*ptr*/, const char* className) {
    // Use shared lock for lookup — the class should already exist
    {
        std::shared_lock lock(_mutex);
        auto it = _stats.find(std::string_view(className));
        if (it != _stats.end()) {
            it->second->liveCount.fetch_sub(1, std::memory_order_relaxed);
            // Note: we don't know the freed size here, so liveBytes drifts.
            // The authoritative byte count comes from Tracy/memoryFootprint().
            // liveCount is always accurate.
            it->second->totalFrees.fetch_add(1, std::memory_order_relaxed);
            it->second->frameFrees.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // Should never happen — free without prior alloc.
    ENTROPY_LOG_WARNING(std::format("MemoryTracker: onFree for unknown class '{}'", className));
}

void MemoryTracker::beginFrame() {
    std::shared_lock lock(_mutex);
    _frameCount++;

    for (auto& [name, stats] : _stats) {
        int64_t allocs = stats->frameAllocs.load(std::memory_order_relaxed);
        int64_t frees = stats->frameFrees.load(std::memory_order_relaxed);
        int64_t live = stats->liveCount.load(std::memory_order_relaxed);
        int64_t liveB = stats->liveBytes.load(std::memory_order_relaxed);

        // Check for excessive per-frame allocations
        if (allocs > _warningThreshold) {
            int64_t netGrowth = allocs - frees;
            ENTROPY_LOG_WARNING(
                std::format("MemoryTracker: '{}' allocated {} objects this frame "
                            "(freed {}, net +{}, live {})",
                            name, allocs, frees, netGrowth, live));

            if (_thresholdCallback) {
                _thresholdCallback(name.data(), allocs, live);
            }
        }

        // Update peaks
        if (live > stats->peakCount) stats->peakCount = live;
        if (liveB > stats->peakBytes) stats->peakBytes = liveB;

        // Reset frame counters
        stats->frameAllocs.store(0, std::memory_order_relaxed);
        stats->frameFrees.store(0, std::memory_order_relaxed);
        stats->frameAllocBytes.store(0, std::memory_order_relaxed);
    }

    // Periodic summary dump
    if (_dumpInterval > 0 && (_frameCount % _dumpInterval) == 0) {
        dumpReport();
    }
}

void MemoryTracker::dumpReport() const {
    struct Entry
    {
        std::string_view name;
        int64_t liveCount;
        int64_t liveBytes;
        int64_t peakCount;
        int64_t peakBytes;
        int64_t totalAllocs;
    };

    std::vector<Entry> entries;

    // If called externally (not from beginFrame), we need the lock.
    // If called from beginFrame, we already hold shared lock.
    // try_lock_shared returns true if we acquired it, false if already held.
    bool acquiredLock = _mutex.try_lock_shared();

    entries.reserve(_stats.size());
    for (const auto& [name, stats] : _stats) {
        int64_t live = stats->liveCount.load(std::memory_order_relaxed);
        if (live == 0 && stats->totalAllocs.load(std::memory_order_relaxed) == 0) continue;

        entries.push_back({name, live, stats->liveBytes.load(std::memory_order_relaxed), stats->peakCount,
                           stats->peakBytes, stats->totalAllocs.load(std::memory_order_relaxed)});
    }

    if (acquiredLock) _mutex.unlock_shared();

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.liveCount > b.liveCount; });

    ENTROPY_LOG_INFO("=== MemoryTracker Report ===");
    ENTROPY_LOG_INFO(std::format("{:<30} {:>8} {:>12} {:>8} {:>12} {:>10}", "Class", "Live", "LiveBytes", "Peak",
                                 "PeakBytes", "TotalAlloc"));

    for (const auto& e : entries) {
        auto fmtBytes = [](int64_t bytes) -> std::string {
            if (bytes >= static_cast<int64_t>(1024) * 1024) {
                return std::format("{:.1f}MB", bytes / (1024.0 * 1024.0));
            }
            if (bytes >= 1024) {
                return std::format("{:.1f}KB", bytes / 1024.0);
            }
            return std::format("{}B", bytes);
        };

        ENTROPY_LOG_INFO(std::format("{:<30} {:>8} {:>12} {:>8} {:>12} {:>10}", e.name, e.liveCount,
                                     fmtBytes(e.liveBytes), e.peakCount, fmtBytes(e.peakBytes), e.totalAllocs));
    }

    ENTROPY_LOG_INFO("=== End MemoryTracker Report ===");
}

const ClassMemoryStats* MemoryTracker::getStats(const char* className) const {
    std::shared_lock lock(_mutex);
    auto it = _stats.find(std::string_view(className));
    if (it != _stats.end()) return it->second.get();
    return nullptr;
}

ClassMemoryStats* MemoryTracker::getOrCreateStats(const char* className) {
    std::string_view key(className);

    // Fast path: shared lock lookup (hot path — most classes already registered)
    {
        std::shared_lock lock(_mutex);
        auto it = _stats.find(key);
        if (it != _stats.end()) return it->second.get();
    }

    // Slow path: exclusive lock for first-time class registration
    {
        std::unique_lock lock(_mutex);
        // Double-check after acquiring exclusive lock
        auto it = _stats.find(key);
        if (it != _stats.end()) return it->second.get();

        auto [inserted, success] = _stats.emplace(key, std::make_unique<ClassMemoryStats>());
        return inserted->second.get();
    }
}

}  // namespace EntropyEngine::Core
