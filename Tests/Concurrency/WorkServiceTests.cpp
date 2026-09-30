#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <future>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkGraph.h"
#include "Concurrency/WorkService.h"
#include "ThreadCpuHelpers.h"

using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::Concurrency::TestSupport;

namespace
{
using Clock = std::chrono::steady_clock;

/// One-shot completion flag with a bounded wait.
struct Latch
{
    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    void set() {
        {
            std::lock_guard<std::mutex> lock(m);
            done = true;
        }
        cv.notify_all();
    }
    bool waitFor(std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, bound, [this] { return done; });
    }
    void reset() {
        std::lock_guard<std::mutex> lock(m);
        done = false;
    }
};

int64_t toMs(Clock::duration d) {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

/// Arms one timed contract and reports when it started.
struct TimedProbe
{
    Latch ran;
    std::atomic<int64_t> startNs{0};
    std::atomic<int> runs{0};

    WorkContractHandle arm(WorkContractGroup& group, Clock::time_point due,
                           ExecutionType type = ExecutionType::AnyThread) {
        auto h = group.createContract(
            [this]() noexcept {
                startNs.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
                runs.fetch_add(1, std::memory_order_acq_rel);
                ran.set();
            },
            type);
        if (h.valid()) {
            h.scheduleAt(due);
        }
        return h;
    }
    Clock::time_point startedAt() const {
        return Clock::time_point(Clock::duration(startNs.load(std::memory_order_acquire)));
    }
};
}  // namespace

TEST(WorkService, ExecutesScheduledWorkOnWorkers) {
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);

    WorkContractGroup group(256, "ServiceTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    EXPECT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Exists);

    service.start();
    EXPECT_TRUE(service.isRunning());
    service.start();  // double-start must be a no-op (regression: check-then-act on _running)

    std::atomic<int> executed{0};
    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        auto h = group.createContract([&executed]() noexcept { executed.fetch_add(1); });
        ASSERT_TRUE(h.valid());
        h.schedule();
    }

    group.wait();
    EXPECT_EQ(executed.load(), N);

    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
    EXPECT_FALSE(service.isRunning());
}

TEST(WorkService, MainThreadWorkPump) {
    WorkService::Config config;
    config.threadCount = 2;
    WorkService service(config);

    WorkContractGroup group(64, "MainPumpTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    std::thread::id mainId = std::this_thread::get_id();
    std::atomic<int> wrongThread{0};
    std::atomic<int> executed{0};

    constexpr int N = 20;
    for (int i = 0; i < N; ++i) {
        auto h = group.createContract(
            [&, mainId]() noexcept {
                if (std::this_thread::get_id() != mainId) wrongThread.fetch_add(1);
                executed.fetch_add(1);
            },
            ExecutionType::MainThread);
        ASSERT_TRUE(h.valid());
        h.schedule();
    }

    // Pump from this thread until drained
    while (executed.load() < N) {
        service.executeMainThreadWork(8);
        std::this_thread::yield();
    }

    EXPECT_EQ(executed.load(), N);
    EXPECT_EQ(wrongThread.load(), 0);  // main-thread work never ran on workers

    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, IdleWorkersDoNotWakeWithoutWork) {
    WorkService::Config config;  // default thread count
    WorkService service(config);
    WorkContractGroup group(64, "IdleCpuTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    std::vector<ThreadId> tids;
#if defined(__linux__)
    tids = captureWorkerTids(service, group);
    ASSERT_EQ(tids.size(), service.getThreadCount());
#endif

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto cpuBefore = poolCpuTime(tids);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto cpuAfter = poolCpuTime(tids);
    ASSERT_GE(cpuBefore.count(), 0);
    ASSERT_GE(cpuAfter.count(), 0);
    const auto cpuUsed = cpuAfter - cpuBefore;

    EXPECT_LT(cpuUsed, std::chrono::milliseconds(25))
        << "threads=" << service.getThreadCount() << " cpu over 500 ms = "
        << std::chrono::duration_cast<std::chrono::milliseconds>(cpuUsed).count() << " ms";

    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, BurstWakesEveryWorker) {
    WorkService::Config config;
    WorkService service(config);
    const size_t threadCount = service.getThreadCount();
    WorkContractGroup group(256, "BurstWakeTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::mutex m;
    std::condition_variable cv;
    size_t arrived = 0;

    std::vector<WorkContractHandle> handles;
    for (size_t i = 0; i < threadCount; ++i) {
        auto h = group.createContract([&]() noexcept {
            std::unique_lock<std::mutex> lock(m);
            if (++arrived == threadCount) {
                cv.notify_all();
            }
            cv.wait_for(lock, std::chrono::seconds(2), [&] { return arrived == threadCount; });
        });
        ASSERT_TRUE(h.valid());
        handles.push_back(h);
    }

    for (auto& h : handles) {
        h.schedule();
    }
    bool allArrived = false;
    {
        std::unique_lock<std::mutex> lock(m);
        allArrived = cv.wait_for(lock, std::chrono::seconds(2), [&] { return arrived == threadCount; });
    }
    EXPECT_TRUE(allArrived) << "threads=" << threadCount << " arrived=" << arrived;

    group.wait();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, NoLostWakeupUnderParkRace) {
    WorkService::Config config;
    WorkService service(config);
    WorkContractGroup group(64, "ParkRaceTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> jitterUs(0, 50);
    std::mutex m;
    std::condition_variable cv;
    uint64_t completed = 0;

    constexpr uint64_t ITERATIONS = 20000;
    uint64_t lostAt = 0;
    for (uint64_t i = 1; i <= ITERATIONS; ++i) {
        auto h = group.createContract([&]() noexcept {
            {
                std::lock_guard<std::mutex> lock(m);
                ++completed;
            }
            cv.notify_all();
        });
        ASSERT_TRUE(h.valid());
        h.schedule();
        {
            std::unique_lock<std::mutex> lock(m);
            if (!cv.wait_for(lock, std::chrono::seconds(1), [&] { return completed == i; })) {
                lostAt = i;
                break;
            }
        }
        const auto spinUntil = Clock::now() + std::chrono::microseconds(jitterUs(rng));
        while (Clock::now() < spinUntil) {
        }
    }
    EXPECT_EQ(lostAt, 0u) << "contract " << lostAt << " did not run within 1 s";

    group.wait();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, GroupRegisteredWithScheduledWorkRuns) {
    WorkService::Config config;
    WorkService service(config);
    service.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    WorkContractGroup group(64, "LateRegisterTest");
    Latch ran;
    auto h = group.createContract([&]() noexcept { ran.set(); });
    ASSERT_TRUE(h.valid());
    h.schedule();

    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    EXPECT_TRUE(ran.waitFor(std::chrono::milliseconds(1000)));

    group.wait();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, MainThreadWaitReturnsOnMainThreadSchedule) {
    WorkService::Config config;
    config.threadCount = 2;
    WorkService service(config);
    WorkContractGroup group(64, "MainWaitScheduleTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    std::atomic<bool> waiting{false};
    std::promise<Clock::time_point> returned;
    auto returnedFuture = returned.get_future();
    std::thread waiter([&] {
        const uint64_t snapshot = service.mainThreadWorkSnapshot();
        waiting.store(true, std::memory_order_release);
        service.waitForMainThreadWork(snapshot);
        returned.set_value(Clock::now());
    });

    while (!waiting.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let the waiter block
    ASSERT_EQ(returnedFuture.wait_for(std::chrono::seconds(0)), std::future_status::timeout);

    std::thread scheduler([&] {
        auto h = group.createContract([]() noexcept {}, ExecutionType::MainThread);
        ASSERT_TRUE(h.valid());
        h.schedule();
    });
    scheduler.join();

    const bool didReturn = returnedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    if (!didReturn) {
        service.notifyMainThreadWorkAvailable();  // release the waiter so the test cannot hang
    }
    waiter.join();
    ASSERT_TRUE(didReturn) << "waitForMainThreadWork did not return after a MainThread schedule";

    EXPECT_EQ(service.executeMainThreadWork().contractsExecuted, 1u);
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, MainThreadWaitBlocksWithoutWork) {
    WorkService::Config config;
    config.threadCount = 2;
    WorkService service(config);
    WorkContractGroup group(64, "MainWaitIdleTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    std::promise<void> returned;
    auto returnedFuture = returned.get_future();
    std::thread waiter([&] {
        const uint64_t snapshot = service.mainThreadWorkSnapshot();
        service.waitForMainThreadWork(snapshot);
        returned.set_value();
    });

    const bool stillBlocked = returnedFuture.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
    service.notifyMainThreadWorkAvailable();
    const bool released = returnedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    if (!released) {
        service.requestStop();  // last resort so the join below cannot hang
    }
    waiter.join();

    EXPECT_TRUE(stillBlocked) << "waitForMainThreadWork returned with no work signalled";
    EXPECT_TRUE(released) << "notifyMainThreadWorkAvailable did not release the waiter";

    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, YieldUntilReleasesOnIdlePool) {
    WorkService::Config config;
    config.threadCount = 2;
    WorkService service(config);
    WorkContractGroup group(64, "YieldUntilIdleTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    std::atomic<int> runs{0};
    {
        WorkGraph graph(&group);
        graph.addYieldableNode(
            [&runs]() -> WorkResultContext {
                if (runs.fetch_add(1) == 0) {
                    return WorkResultContext::yieldUntil(Clock::now() + std::chrono::milliseconds(50));
                }
                return WorkResultContext::complete();
            },
            "yield-until-once");

        graph.execute();

        // Watchdog: poll completion for at most 2 s so a missed deadline cannot hang the test.
        Latch completed;
        std::thread watcher([&] {
            const auto bound = Clock::now() + std::chrono::seconds(2);
            while (Clock::now() < bound) {
                if (graph.isComplete()) {
                    completed.set();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        const bool done = completed.waitFor(std::chrono::milliseconds(2100));
        watcher.join();

        ASSERT_TRUE(done) << "yieldUntil node never re-ran on an idle pool (runs=" << runs.load() << ")";
        EXPECT_EQ(runs.load(), 2);
    }

    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, TimedContractFiresOnIdlePool) {
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    WorkContractGroup group(64, "TimedIdleTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TimedProbe probe;
    const auto armedAt = Clock::now();
    ASSERT_TRUE(probe.arm(group, armedAt + std::chrono::milliseconds(50)).valid());
    ASSERT_TRUE(probe.ran.waitFor(std::chrono::milliseconds(2000)))
        << "a 50 ms timed contract did not run within 2 s on an idle pool";
    EXPECT_GE(toMs(probe.startedAt() - armedAt), 50);  // never early

    group.wait();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, TimedContractEarlierDeadlineRetargets) {
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    WorkContractGroup group(64, "TimedRetargetTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TimedProbe far;
    auto farHandle = far.arm(group, Clock::now() + std::chrono::seconds(5));
    ASSERT_TRUE(farHandle.valid());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let a worker hold the 5 s deadline

    TimedProbe near;
    const auto armedAt = Clock::now();
    ASSERT_TRUE(near.arm(group, armedAt + std::chrono::milliseconds(50)).valid());
    ASSERT_TRUE(near.ran.waitFor(std::chrono::milliseconds(2000)))
        << "the 50 ms contract waited behind the 5 s deadline";
    EXPECT_GE(toMs(near.startedAt() - armedAt), 50);  // never early
    EXPECT_EQ(far.runs.load(), 0);

    EXPECT_EQ(farHandle.unschedule(), ScheduleResult::NotScheduled);
    farHandle.release();
    group.wait();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, TimedContractArmedFromWorkerFires) {
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    WorkContractGroup group(64, "TimedFromWorkerTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TimedProbe probe;
    std::atomic<int64_t> armedNs{0};
    auto armer = group.createContract([&]() noexcept {
        const auto now = Clock::now();
        armedNs.store(now.time_since_epoch().count(), std::memory_order_release);
        probe.arm(group, now + std::chrono::milliseconds(30));
    });
    ASSERT_TRUE(armer.valid());
    armer.schedule();

    ASSERT_TRUE(probe.ran.waitFor(std::chrono::milliseconds(2000)))
        << "a timed contract armed from a worker did not run within 2 s";
    const auto armedAt = Clock::time_point(Clock::duration(armedNs.load(std::memory_order_acquire)));
    EXPECT_GE(toMs(probe.startedAt() - armedAt), 30);  // never early

    group.wait();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, TimedMainThreadContractWakesMainThreadWait) {
    WorkService::Config config;
    config.threadCount = 2;
    WorkService service(config);
    WorkContractGroup group(64, "TimedMainThreadTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    std::atomic<bool> stopLoop{false};
    std::atomic<bool> looping{false};
    std::thread::id loopId;
    std::thread loop([&] {
        loopId = std::this_thread::get_id();
        looping.store(true, std::memory_order_release);
        while (!stopLoop.load(std::memory_order_acquire)) {
            const uint64_t snapshot = service.mainThreadWorkSnapshot();
            const auto result = service.executeMainThreadWork();
            if (!result.moreWorkAvailable && !stopLoop.load(std::memory_order_acquire)) {
                service.waitForMainThreadWork(snapshot, result.nextDue);
            }
        }
    });
    while (!looping.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the loop block

    TimedProbe probe;
    std::atomic<bool> onLoop{false};
    const auto armedAt = Clock::now();
    auto h = group.createContract(
        [&]() noexcept {
            onLoop.store(std::this_thread::get_id() == loopId, std::memory_order_release);
            probe.startNs.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
            probe.ran.set();
        },
        ExecutionType::MainThread);
    ASSERT_TRUE(h.valid());
    h.scheduleAt(armedAt + std::chrono::milliseconds(50));

    const bool didRun = probe.ran.waitFor(std::chrono::milliseconds(2000));
    stopLoop.store(true, std::memory_order_release);
    service.notifyMainThreadWorkAvailable();
    loop.join();

    ASSERT_TRUE(didRun) << "a 50 ms MainThread timed contract did not release waitForMainThreadWork";
    EXPECT_TRUE(onLoop.load());
    EXPECT_GE(toMs(probe.startedAt() - armedAt), 50);  // never early

    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, IdlePoolWithNoTimedWorkWakesNoWorker) {
#if !defined(__linux__)
    GTEST_SKIP() << "reads /proc/self/task";
#else
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    WorkContractGroup group(64, "IdleSwitchTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    const auto tids = captureWorkerTids(service, group);
    ASSERT_EQ(tids.size(), service.getThreadCount());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const long before = totalVoluntarySwitches(tids);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const long after = totalVoluntarySwitches(tids);
    ASSERT_GE(before, 0);
    ASSERT_GE(after, 0);
    EXPECT_LE(after - before, 2) << "workers=" << tids.size() << " voluntary switches over 500 ms";

    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
#endif
}

TEST(WorkService, IdlePoolWithFarTimedWorkWakesNoWorker) {
#if !defined(__linux__)
    GTEST_SKIP() << "reads /proc/self/task";
#else
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    WorkContractGroup group(64, "FarTimedSwitchTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    const auto tids = captureWorkerTids(service, group);
    ASSERT_EQ(tids.size(), service.getThreadCount());

    TimedProbe far;
    auto farHandle = far.arm(group, Clock::now() + std::chrono::seconds(30));
    ASSERT_TRUE(farHandle.valid());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const long before = totalVoluntarySwitches(tids);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const long after = totalVoluntarySwitches(tids);
    ASSERT_GE(before, 0);
    ASSERT_GE(after, 0);
    EXPECT_LE(after - before, 2) << "workers=" << tids.size() << " voluntary switches over 500 ms";
    EXPECT_EQ(far.runs.load(), 0);

    EXPECT_EQ(farHandle.unschedule(), ScheduleResult::NotScheduled);
    farHandle.release();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
#endif
}

TEST(WorkService, DeadlineWakesOneWorker) {
#if !defined(__linux__)
    GTEST_SKIP() << "reads /proc/self/task";
#else
    WorkService::Config config;
    config.threadCount = 8;
    WorkService service(config);
    WorkContractGroup group(64, "DeadlineHerdTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    const auto tids = captureWorkerTids(service, group);
    ASSERT_EQ(tids.size(), service.getThreadCount());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    constexpr int FIRES = 10;
    std::atomic<int> fired{0};
    std::atomic<int> early{0};
    const long before = totalVoluntarySwitches(tids);
    const auto base = Clock::now();
    for (int i = 1; i <= FIRES; ++i) {
        const auto due = base + std::chrono::milliseconds(30 * i);
        auto h = group.createContract([&fired, &early, due]() noexcept {
            if (Clock::now() < due) early.fetch_add(1, std::memory_order_relaxed);
            fired.fetch_add(1, std::memory_order_release);
        });
        ASSERT_TRUE(h.valid());
        h.scheduleAt(due);
    }
    const auto bound = base + std::chrono::milliseconds(30 * FIRES + 1000);
    while (fired.load(std::memory_order_acquire) < FIRES && Clock::now() < bound) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const long after = totalVoluntarySwitches(tids);
    ASSERT_GE(before, 0);
    ASSERT_GE(after, 0);
    ASSERT_EQ(fired.load(), FIRES);
    EXPECT_EQ(early.load(), 0);
    EXPECT_LE(after - before, FIRES * 4 + static_cast<long>(tids.size()))
        << "workers=" << tids.size() << " voluntary switches for " << FIRES << " fires";

    group.wait();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
#endif
}

TEST(WorkService, TimedContractFiresWhileWorkersBusy) {
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    const size_t threadCount = service.getThreadCount();
    WorkContractGroup group(64, "TimedBusyTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::atomic<size_t> busy{0};
    for (size_t i = 0; i < threadCount; ++i) {
        auto h = group.createContract([&busy]() noexcept {
            busy.fetch_add(1, std::memory_order_acq_rel);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });
        ASSERT_TRUE(h.valid());
        h.schedule();
    }
    while (busy.load(std::memory_order_acquire) < threadCount) {
        std::this_thread::yield();
    }

    TimedProbe probe;
    const auto armedAt = Clock::now();
    ASSERT_TRUE(probe.arm(group, armedAt + std::chrono::milliseconds(50)).valid());
    ASSERT_TRUE(probe.ran.waitFor(std::chrono::milliseconds(2000)))
        << "a timed contract did not run once busy workers finished";
    EXPECT_GE(toMs(probe.startedAt() - armedAt), 50);  // never early

    group.wait();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, TimedContractsNeverRunEarly) {
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    WorkContractGroup group(1024, "TimedNeverEarlyTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    constexpr int N = 500;
    std::mt19937 rng(424242);
    std::uniform_int_distribution<int> dueMs(1, 100);
    std::vector<int64_t> dueNs(N, 0);
    std::vector<int64_t> startNs(N, 0);
    std::atomic<int> ran{0};

    for (int i = 0; i < N; ++i) {
        const auto due = Clock::now() + std::chrono::milliseconds(dueMs(rng));
        dueNs[i] = due.time_since_epoch().count();
        auto h = group.createContract([&, i]() noexcept {
            startNs[i] = Clock::now().time_since_epoch().count();
            ran.fetch_add(1, std::memory_order_release);
        });
        ASSERT_TRUE(h.valid());
        h.scheduleAt(due);
    }

    const auto bound = Clock::now() + std::chrono::seconds(2);
    while (ran.load(std::memory_order_acquire) < N && Clock::now() < bound) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(ran.load(std::memory_order_acquire), N) << "timed contracts still pending after 2 s";

    int early = 0;
    for (int i = 0; i < N; ++i) {
        if (startNs[i] < dueNs[i]) ++early;
    }
    EXPECT_EQ(early, 0);

    group.wait();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, GroupWithOnlyFutureTimedWorkDoesNotStarveOtherGroups) {
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    // The timed group is registered first, so a scheduler that sticks to it sees it first.
    WorkContractGroup timedGroup(64, "FutureTimedGroup");
    WorkContractGroup otherGroup(64, "OrdinaryGroup");
    ASSERT_EQ(service.addWorkContractGroup(&timedGroup), WorkService::GroupOperationStatus::Added);
    ASSERT_EQ(service.addWorkContractGroup(&otherGroup), WorkService::GroupOperationStatus::Added);
    service.start();

    TimedProbe future;
    auto futureHandle = future.arm(timedGroup, Clock::now() + std::chrono::hours(1));
    ASSERT_TRUE(futureHandle.valid());

    for (int i = 0; i < 20; ++i) {
        Latch ran;
        auto h = otherGroup.createContract([&ran]() noexcept { ran.set(); });
        ASSERT_TRUE(h.valid());
        h.schedule();
        // The hour-away contract is the only other work, so running at all means it did not block.
        ASSERT_TRUE(ran.waitFor(std::chrono::seconds(5))) << "contract " << i << " starved behind the timed group";
        otherGroup.wait();
    }
    EXPECT_EQ(future.runs.load(), 0);

    EXPECT_EQ(futureHandle.unschedule(), ScheduleResult::NotScheduled);
    futureHandle.release();
    ASSERT_EQ(service.removeWorkContractGroup(&otherGroup), WorkService::GroupOperationStatus::Removed);
    ASSERT_EQ(service.removeWorkContractGroup(&timedGroup), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, TimedContractRunsWhileLongContractHoldsAWorker) {
    WorkService::Config config;
    config.threadCount = 2;
    WorkService service(config);
    WorkContractGroup group(64, "DeadlineHandoffTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    for (int iteration = 0; iteration < 6; ++iteration) {
        TimedProbe timed;
        ASSERT_TRUE(timed.arm(group, Clock::now() + std::chrono::milliseconds(100)).valid());

        // A second contract wakes the other worker, which learns the due time and parks.
        auto brief = group.createContract([]() noexcept {});
        ASSERT_TRUE(brief.valid());
        brief.schedule();

        // The long contract holds one of the two workers until the timed contract has run.
        std::atomic<bool> timedRanFirst{false};
        Latch longStarted;
        auto longRunning = group.createContract([&]() noexcept {
            longStarted.set();
            timedRanFirst.store(timed.ran.waitFor(std::chrono::seconds(5)), std::memory_order_release);
        });
        ASSERT_TRUE(longRunning.valid());
        longRunning.schedule();

        ASSERT_TRUE(longStarted.waitFor(std::chrono::seconds(5)));
        group.wait();
        EXPECT_TRUE(timedRanFirst.load(std::memory_order_acquire))
            << "iteration " << iteration << ": the timed contract waited for the long contract";
    }

    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(WorkService, WorkerThreadCountIsFixed) {
#if !defined(__linux__)
    GTEST_SKIP() << "reads /proc/self/task";
#else
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    WorkContractGroup group(256, "ThreadCountTest");
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const size_t threadsBefore = processThreadCount();

    constexpr int N = 100;
    std::atomic<int> ran{0};
    const auto base = Clock::now();
    for (int i = 0; i < N; ++i) {
        auto h = group.createContract([&ran]() noexcept { ran.fetch_add(1, std::memory_order_release); });
        ASSERT_TRUE(h.valid());
        h.scheduleAt(base + std::chrono::milliseconds(1 + i));
    }
    const auto bound = Clock::now() + std::chrono::seconds(2);
    while (ran.load(std::memory_order_acquire) < N && Clock::now() < bound) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const size_t threadsAfter = processThreadCount();
    ASSERT_EQ(ran.load(), N);
    EXPECT_EQ(threadsAfter, threadsBefore);

    group.wait();
    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
#endif
}
