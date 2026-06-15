/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Entropy Engine Contributors
 * This file is part of the EntropyCore project.
 */

/**
 * @file CpuZoneProfiler.h
 * @brief Console-readable mirror of the Tracy CPU zones.
 *
 * Lives in EntropyCore (the lowest layer) so BOTH EntropyCore and the layers
 * above it (EntropyRender, etc.) can feed it — the per-frame main-thread-work
 * cost (`WorkService::executeMainThreadWork`, `WorkContractGroup::
 * executeContract`) is incurred in EntropyCore, below where the render-side
 * profiler used to live, so it could not be attributed headlessly before.
 *
 * The render-side `EPZoneScopedN`/`EPZoneScopedNC` macros (EntropyRender's
 * TracyHelpers.h) feed BOTH Tracy (the GUI timeline) and this profiler, which
 * accumulates per-zone wall-clock time per frame and exposes it via
 * `state get cpu.zones` — so the same breakdown a human sees in Tracy is
 * readable headlessly (agents, CI, scripts) WITHOUT a Tracy GUI attached.
 * EntropyCore code uses CpuZoneScope directly (it has no TracyHelpers.h).
 *
 * Single instance: `get()` returns a function-local static defined once in the
 * EntropyCore TU, so every layer that links EntropyCore shares ONE profiler and
 * ONE `sEnabled` gate — worker threads spawned anywhere accumulate into it.
 *
 * Cost: gated by `setEnabled` (default OFF → a single relaxed-atomic load per
 * zone, no timing, no allocation). When ON, accumulation is thread-local and
 * lock-free on the hot path; per-thread maps are merged on the main thread at
 * `beginFrame`. NOTE: when enabled, very hot (per-draw) zones self-inflate
 * slightly from the timing/map overhead — read the coarse phase zones for
 * attribution, not absolute per-draw cost.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EntropyEngine::Core::Debug
{

class CpuZoneProfiler
{
public:
    static CpuZoneProfiler& get();

    /// Cheap gate read — checked by every zone scope. Default false.
    static bool enabled() noexcept {
        return sEnabled.load(std::memory_order_relaxed);
    }
    void setEnabled(bool e) noexcept {
        sEnabled.store(e, std::memory_order_relaxed);
    }

    /// Accumulate `ms` into the calling thread's per-zone map (lock-free after
    /// first-touch registration). No-op caller-side when disabled.
    void add(const char* name, double ms);

    /// Merge every thread's accumulator into the published snapshot and clear
    /// them. Call once per frame on the main thread AFTER all worker recording
    /// for the prior frame has completed (e.g. top of FrameOrchestrator frame).
    void beginFrame();

    /// Published last-frame totals, sorted descending by milliseconds.
    [[nodiscard]] std::vector<std::pair<std::string, double>> snapshot() const;

private:
    static std::atomic<bool> sEnabled;

    struct ThreadAccum
    {
        std::mutex mtx;  // guards `map`: the owning thread inserts via add() while
                         // beginFrame() (main thread) merges+clears it — without
                         // this they race and corrupt the map (SIGSEGV). Uncontended
                         // on the hot path (only beginFrame contends, once/frame).
        std::unordered_map<std::string, double> map;
    };
    ThreadAccum& tl();  // thread-local, registers itself with `_all` on first touch

    std::mutex _regMutex;                 // guards `_all`
    std::vector<ThreadAccum*> _all;       // every thread's accumulator (leaked at thread exit)
    mutable std::mutex _lastMutex;        // guards `_last`
    std::unordered_map<std::string, double> _last;  // published prior-frame totals
};

/// RAII scope: times its lifetime and accumulates into the profiler when
/// enabled. Instantiated by the EPZoneScoped* macros (EntropyRender) and
/// directly by EntropyCore code (which has no TracyHelpers.h).
class CpuZoneScope
{
public:
    explicit CpuZoneScope(const char* name) noexcept : _name(name), _active(CpuZoneProfiler::enabled()) {
        if (_active) _t0 = std::chrono::high_resolution_clock::now();
    }
    ~CpuZoneScope() {
        if (_active) {
            const auto t1 = std::chrono::high_resolution_clock::now();
            CpuZoneProfiler::get().add(_name, std::chrono::duration<double, std::milli>(t1 - _t0).count());
        }
    }
    CpuZoneScope(const CpuZoneScope&) = delete;
    CpuZoneScope& operator=(const CpuZoneScope&) = delete;

private:
    const char* _name;
    std::chrono::high_resolution_clock::time_point _t0;
    bool _active;
};

}  // namespace EntropyEngine::Core::Debug
