/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Entropy Engine Contributors
 * This file is part of the EntropyCore project.
 */

#include "CpuZoneProfiler.h"

#include <algorithm>

namespace EntropyEngine::Core::Debug
{

std::atomic<bool> CpuZoneProfiler::sEnabled{false};

CpuZoneProfiler& CpuZoneProfiler::get() {
    static CpuZoneProfiler sInstance;
    return sInstance;
}

CpuZoneProfiler::ThreadAccum& CpuZoneProfiler::tl() {
    // One accumulator per thread, registered once. Leaked on thread exit —
    // worker threads are persistent for the process lifetime, so this is a
    // bounded, one-time allocation per thread.
    static thread_local ThreadAccum* sLocal = nullptr;
    if (!sLocal) {
        sLocal = new ThreadAccum();
        std::lock_guard<std::mutex> lk(_regMutex);
        _all.push_back(sLocal);
    }
    return *sLocal;
}

void CpuZoneProfiler::add(const char* name, double ms) {
    // Per-thread map, but beginFrame() (main thread) merges+clears it
    // concurrently, so guard with the per-thread mutex. Uncontended except for
    // the once-per-frame beginFrame pass. (Registration of a new thread's map
    // takes `_regMutex` once on first touch.)
    ThreadAccum& a = tl();
    std::lock_guard<std::mutex> lk(a.mtx);
    a.map[name] += ms;
}

void CpuZoneProfiler::beginFrame() {
    std::lock_guard<std::mutex> reg(_regMutex);
    std::lock_guard<std::mutex> last(_lastMutex);
    _last.clear();
    for (ThreadAccum* a : _all) {
        // Lock the owning thread's map — it may be concurrently inserting via
        // add() (executeContract runs on worker threads continuously, not
        // frame-synced). Without this the merge+clear races the insert → SIGSEGV.
        std::lock_guard<std::mutex> lk(a->mtx);
        for (auto& [k, v] : a->map) _last[k] += v;
        a->map.clear();
    }
}

std::vector<std::pair<std::string, double>> CpuZoneProfiler::snapshot() const {
    std::vector<std::pair<std::string, double>> out;
    {
        std::lock_guard<std::mutex> last(_lastMutex);
        out.assign(_last.begin(), _last.end());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

}  // namespace EntropyEngine::Core::Debug
