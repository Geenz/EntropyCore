/**
 * @file WorkServiceBenchmarks.cpp
 * @brief Throughput benchmarks for WorkService contract selection
 *
 * Disabled by default; they only print timings. Run with:
 *   EntropyCoreTests --gtest_also_run_disabled_tests --gtest_filter='*WorkServiceBenchmark*'
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkService.h"

using namespace EntropyEngine::Core::Concurrency;

namespace
{
using Clock = std::chrono::steady_clock;

constexpr int RUNS = 5;
constexpr size_t GROUP_CAPACITY = 65536;

/// One workload: how many tiny and how many 1 ms contracts, interleaved.
struct Workload
{
    const char* name;
    size_t tinyContracts;
    size_t sleepContracts;
};

/// Schedules the workload on a running service and returns the time until every contract ran.
Clock::duration runOnce(WorkService& service, const Workload& workload) {
    WorkContractGroup group(GROUP_CAPACITY, "Benchmark");
    EXPECT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);

    std::atomic<size_t> sink{0};
    const size_t total = workload.tinyContracts + workload.sleepContracts;
    // Spread the sleeping contracts evenly through the tiny ones.
    const size_t sleepEvery = workload.sleepContracts ? std::max<size_t>(1, total / workload.sleepContracts) : 0;

    const auto begin = Clock::now();
    size_t tiny = 0;
    size_t sleeping = 0;
    for (size_t i = 0; i < total; ++i) {
        const bool sleeps = sleeping < workload.sleepContracts &&
                            (tiny >= workload.tinyContracts || (sleepEvery && i % sleepEvery == 0));
        WorkContractHandle h;
        do {
            h = sleeps ? group.createContract([]() noexcept { std::this_thread::sleep_for(std::chrono::milliseconds(1)); })
                       : group.createContract([&sink]() noexcept { sink.fetch_add(1, std::memory_order_relaxed); });
            if (!h.valid()) std::this_thread::yield();  // group full; workers free slots as they run
        } while (!h.valid());
        EXPECT_EQ(h.schedule(), ScheduleResult::Scheduled);
        (sleeps ? sleeping : tiny)++;
    }
    group.wait();
    const auto elapsed = Clock::now() - begin;

    EXPECT_EQ(sink.load(), workload.tinyContracts);
    EXPECT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    return elapsed;
}

void benchmark(const Workload& workload) {
    WorkService service(WorkService::Config{});
    service.start();

    std::vector<double> ms;
    for (int run = 0; run < RUNS; ++run) {
        ms.push_back(std::chrono::duration<double, std::milli>(runOnce(service, workload)).count());
    }
    service.stop();

    std::sort(ms.begin(), ms.end());
    std::printf("[WorkServiceBenchmark] %-16s workers=%zu tiny=%zu sleep1ms=%zu  median=%.2f ms  min=%.2f ms  max=%.2f ms\n",
                workload.name, service.getThreadCount(), workload.tinyContracts, workload.sleepContracts,
                ms[ms.size() / 2], ms.front(), ms.back());
}
}  // namespace

TEST(WorkServiceBenchmark, DISABLED_HighContentionTinyContracts) {
    benchmark({"high-contention", 200000, 0});
}

TEST(WorkServiceBenchmark, DISABLED_LowContentionSleepingContracts) {
    benchmark({"low-contention", 0, 4000});
}

TEST(WorkServiceBenchmark, DISABLED_MixedContention) {
    benchmark({"mixed", 100000, 2000});
}
