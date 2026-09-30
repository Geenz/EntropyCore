/**
 * @file ThreadCpuHelpers.h
 * @brief Per-thread CPU accounting for idle-CPU tests
 *
 * On Linux, idle CPU is the sum over the WorkService worker threads plus the calling thread, read from
 * /proc/self/task. Other threads in the process (profiler, logger) do not count. Other platforms use process CPU.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkService.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#if defined(__linux__)
#include <dirent.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#endif

namespace EntropyEngine::Core::Concurrency::TestSupport
{

/// Kernel thread id (Linux tid).
using ThreadId = int;

/// CPU time consumed by every thread of this process.
inline std::chrono::nanoseconds processCpuTime() {
#if defined(_WIN32)
    FILETIME creation, exitTime, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user);
    auto toNs = [](const FILETIME& ft) {
        const uint64_t ticks = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100 ns units
        return std::chrono::nanoseconds(ticks * 100);
    };
    return toNs(kernel) + toNs(user);
#else
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
#endif
}

#if defined(__linux__)
/// Kernel thread ids of every worker: one barrier contract per worker, each held until all arrive.
inline std::vector<ThreadId> captureWorkerTids(WorkService& service, WorkContractGroup& group) {
    const size_t threadCount = service.getThreadCount();
    std::mutex m;
    std::condition_variable cv;
    std::vector<ThreadId> tids;
    for (size_t i = 0; i < threadCount; ++i) {
        auto h = group.createContract([&]() noexcept {
            std::unique_lock<std::mutex> lock(m);
            tids.push_back(static_cast<ThreadId>(syscall(SYS_gettid)));
            if (tids.size() == threadCount) {
                cv.notify_all();
            }
            cv.wait_for(lock, std::chrono::seconds(2), [&] { return tids.size() == threadCount; });
        });
        if (!h.valid()) return {};
        h.schedule();
    }
    group.wait();
    std::sort(tids.begin(), tids.end());
    tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
    return tids;
}

/// utime + stime of one thread of this process (/proc/self/task/<tid>/stat fields 14 and 15); nullopt when unreadable.
inline std::optional<std::chrono::nanoseconds> threadCpuTime(ThreadId tid) {
    std::ifstream stat("/proc/self/task/" + std::to_string(tid) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) return std::nullopt;
    // The comm field is parenthesised and may contain spaces; the numeric fields start after the last ')'.
    const size_t close = line.rfind(')');
    if (close == std::string::npos) return std::nullopt;
    std::istringstream fields(line.substr(close + 1));
    std::string field;
    uint64_t utime = 0;
    uint64_t stime = 0;
    int lastIndex = 2;
    for (int index = 3; index <= 15 && (fields >> field); ++index) {  // first token after ')' is field 3
        lastIndex = index;
        if (index == 14) utime = std::stoull(field);
        if (index == 15) stime = std::stoull(field);
    }
    if (lastIndex < 15) return std::nullopt;
    static const long ticksPerSecond = sysconf(_SC_CLK_TCK);
    const uint64_t ticks = utime + stime;
    return std::chrono::nanoseconds(ticks * 1000000000ull / static_cast<uint64_t>(ticksPerSecond));
}

/// voluntary_ctxt_switches of one thread of this process; -1 when unreadable.
inline long voluntarySwitches(ThreadId tid) {
    std::ifstream status("/proc/self/task/" + std::to_string(tid) + "/status");
    std::string line;
    const std::string key = "voluntary_ctxt_switches:";
    while (std::getline(status, line)) {
        if (line.rfind(key, 0) == 0) {
            return std::stol(line.substr(key.size()));
        }
    }
    return -1;
}

/// Sum of voluntarySwitches() over @p tids; -1 when any thread cannot be read.
inline long totalVoluntarySwitches(const std::vector<ThreadId>& tids) {
    long total = 0;
    for (ThreadId tid : tids) {
        const long n = voluntarySwitches(tid);
        if (n < 0) return -1;
        total += n;
    }
    return total;
}

/// Number of threads in this process (entries in /proc/self/task).
inline size_t processThreadCount() {
    size_t count = 0;
    if (DIR* dir = opendir("/proc/self/task")) {
        while (dirent* entry = readdir(dir)) {
            if (entry->d_name[0] != '.') ++count;
        }
        closedir(dir);
    }
    return count;
}
#endif

/// CPU time attributable to the pool under test. Linux: the given worker tids plus the calling thread.
/// Other platforms: the whole process. Returns -1 ns when a thread cannot be read.
inline std::chrono::nanoseconds poolCpuTime([[maybe_unused]] std::vector<ThreadId> tids) {
#if defined(__linux__)
    tids.push_back(static_cast<ThreadId>(syscall(SYS_gettid)));
    std::chrono::nanoseconds total{0};
    for (ThreadId tid : tids) {
        const auto cpu = threadCpuTime(tid);
        if (!cpu) return std::chrono::nanoseconds(-1);
        total += *cpu;
    }
    return total;
#else
    return processCpuTime();
#endif
}

}  // namespace EntropyEngine::Core::Concurrency::TestSupport
