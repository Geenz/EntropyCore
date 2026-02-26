/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file MemoryTracker.h
 * @brief Per-class allocation tracking with frame-scoped rate detection
 *
 * Hooks into EntropyObjectMemoryHooks to maintain per-class statistics:
 * live count, live bytes, peak values, and per-frame allocation rates.
 * Designed to catch per-frame allocation leaks early — if any class
 * exceeds a configurable threshold of allocations per frame, a warning
 * is logged immediately.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>

namespace EntropyEngine::Core
{

/**
 * @brief Per-class memory statistics tracked by MemoryTracker
 *
 * Atomics are used for liveCount/liveBytes/frameAllocs since alloc/free
 * fire from arbitrary threads. Peak values are updated at frame boundaries
 * under the tracker's lock.
 */
struct ClassMemoryStats
{
    std::atomic<int64_t> liveCount{0};        ///< Currently live objects
    std::atomic<int64_t> liveBytes{0};        ///< Currently live bytes (alloc-side only, drifts on free)
    std::atomic<int64_t> totalAllocs{0};      ///< Cumulative allocations
    std::atomic<int64_t> totalFrees{0};       ///< Cumulative frees
    std::atomic<int64_t> frameAllocs{0};      ///< Allocations this frame
    std::atomic<int64_t> frameFrees{0};       ///< Frees this frame
    std::atomic<int64_t> frameAllocBytes{0};  ///< Bytes allocated this frame

    int64_t peakCount = 0;  ///< Highest live count observed
    int64_t peakBytes = 0;  ///< Highest live bytes observed
};

/**
 * @brief Singleton allocation tracker that monitors EntropyObject lifecycle
 *
 * Install via MemoryTracker::install() before setting up other hooks (Tracy, etc).
 * The install() method captures any pre-existing hooks and chains to them.
 * Call beginFrame() once per frame to compute per-frame deltas and emit warnings.
 *
 * Thread-safe: onAlloc/onFree use atomics for hot-path updates.
 * The class map uses a shared_mutex — reads (lookups) are concurrent,
 * writes (new class first seen) are exclusive but rare.
 */
class MemoryTracker
{
public:
    static MemoryTracker& instance();

    /**
     * @brief Install the tracker into EntropyObjectMemoryHooks
     *
     * Wraps any existing onAlloc/onFree callbacks, chaining the tracker
     * before the original callback. Safe to call multiple times (no-op
     * if already installed).
     */
    void install();

    /**
     * @brief Called by the hooks — record an allocation
     */
    void onAlloc(void* ptr, size_t size, const char* className);

    /**
     * @brief Called by the hooks — record a deallocation
     */
    void onFree(void* ptr, const char* className);

    /**
     * @brief Frame boundary — check per-frame rates and reset counters
     *
     * For each class:
     *  1. If frameAllocs > warningThreshold, log a warning
     *  2. Update peak counts
     *  3. Reset frame counters
     *
     * Call once per frame from the main loop.
     */
    void beginFrame();

    /**
     * @brief Log a summary of all tracked classes to ENTROPY_LOG_INFO
     *
     * Outputs: className, liveCount, liveBytes, peakCount, peakBytes, totalAllocs
     * Sorted by live count descending.
     */
    void dumpReport() const;

    /**
     * @brief Get stats for a specific class (nullptr if never seen)
     */
    const ClassMemoryStats* getStats(const char* className) const;

    /**
     * @brief Set per-frame allocation count warning threshold
     *
     * If any class allocates more than this many objects in a single frame,
     * a warning is logged. Default: 50.
     */
    void setWarningThreshold(int64_t count) {
        _warningThreshold = count;
    }
    int64_t warningThreshold() const {
        return _warningThreshold;
    }

    /**
     * @brief Set interval (in frames) between periodic summary dumps
     *
     * Set to 0 to disable periodic dumps. Default: 0 (disabled).
     */
    void setDumpInterval(uint64_t frames) {
        _dumpInterval = frames;
    }

    /**
     * @brief Current frame number (incremented by beginFrame)
     */
    uint64_t frameCount() const {
        return _frameCount;
    }

    /**
     * @brief Optional callback fired when a per-frame threshold is exceeded
     *
     * Signature: void(const char* className, int64_t frameAllocs, int64_t liveCount)
     * Called from beginFrame() under the shared lock.
     */
    using ThresholdCallback = std::function<void(const char* className, int64_t frameAllocs, int64_t liveCount)>;
    void setThresholdCallback(ThresholdCallback cb) {
        _thresholdCallback = std::move(cb);
    }

private:
    MemoryTracker() = default;

    ClassMemoryStats* getOrCreateStats(const char* className);

    mutable std::shared_mutex _mutex;
    std::unordered_map<std::string_view, std::unique_ptr<ClassMemoryStats>> _stats;

    int64_t _warningThreshold = 50;
    uint64_t _dumpInterval = 0;
    uint64_t _frameCount = 0;
    bool _installed = false;

    ThresholdCallback _thresholdCallback;

    // Chained original hooks (Tracy, etc.)
    using AllocCallback = void (*)(void* ptr, size_t size, const char* className);
    using FreeCallback = void (*)(void* ptr, const char* className);
    AllocCallback _chainedAlloc = nullptr;
    FreeCallback _chainedFree = nullptr;
};

}  // namespace EntropyEngine::Core
