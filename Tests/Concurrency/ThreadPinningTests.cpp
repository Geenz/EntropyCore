#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkGraph.h"
#include "Concurrency/WorkService.h"

using namespace EntropyEngine::Core::Concurrency;

TEST(ThreadPinning, InvalidLane_FailsLoudAtCreation) {
    WorkContractGroup group(64, "PinNoLanes");  // maxPinnedLanes defaults to 0
    auto h = group.createContract([]() noexcept {}, ExecutionType::PinnedThread, 0);
    EXPECT_FALSE(h.valid());

    WorkContractGroup lanes(64, "PinTwoLanes", 2);
    EXPECT_EQ(lanes.maxPinnedLanes(), 2u);
    EXPECT_FALSE(lanes.createContract([]() noexcept {}, ExecutionType::PinnedThread, 2).valid());
    auto ok = lanes.createContract([]() noexcept {}, ExecutionType::PinnedThread, 1);
    EXPECT_TRUE(ok.valid());
    ok.release();
}

TEST(ThreadPinning, BackgroundDrainDoesNotTouchPinnedWork) {
    WorkContractGroup group(64, "PinIsolation", 2);
    std::atomic<int> pinnedRan{0};
    std::atomic<int> bgRan{0};

    auto hp = group.createContract([&]() noexcept { pinnedRan.fetch_add(1); }, ExecutionType::PinnedThread, 0);
    auto hb = group.createContract([&]() noexcept { bgRan.fetch_add(1); });
    ASSERT_EQ(hp.schedule(), ScheduleResult::Scheduled);
    ASSERT_EQ(hb.schedule(), ScheduleResult::Scheduled);

    EXPECT_TRUE(group.hasPinnedWork(0));
    EXPECT_FALSE(group.hasPinnedWork(1));

    // Background drain must leave the pinned lane untouched
    group.executeAllBackgroundWork();
    EXPECT_EQ(bgRan.load(), 1);
    EXPECT_EQ(pinnedRan.load(), 0);
    EXPECT_TRUE(group.hasPinnedWork(0));

    // The lane's pump drains it
    EXPECT_EQ(group.executePinnedWork(0), 1u);
    EXPECT_EQ(pinnedRan.load(), 1);
    EXPECT_FALSE(group.hasPinnedWork(0));

    group.wait();
    EXPECT_EQ(group.activeCount(), 0u);
}

TEST(ThreadPinning, ExternalThreadPumpsItsOwnLane) {
    WorkContractGroup group(64, "PinExternal", 1);
    std::atomic<int> executed{0};
    std::atomic<bool> stop{false};
    std::thread::id laneThreadId;
    std::atomic<int> wrongThread{0};

    // The lane-owning thread: pumps lane 0 until told to stop
    std::thread laneThread([&]() {
        laneThreadId = std::this_thread::get_id();
        while (!stop.load(std::memory_order_acquire)) {
            group.executePinnedWork(0);
            std::this_thread::yield();
        }
        group.executePinnedWork(0);
    });

    // Another thread schedules work pinned to lane 0
    constexpr int N = 50;
    for (int i = 0; i < N; ++i) {
        auto h = group.createContract(
            [&]() noexcept {
                if (std::this_thread::get_id() != laneThreadId) wrongThread.fetch_add(1);
                executed.fetch_add(1);
            },
            ExecutionType::PinnedThread, 0);
        ASSERT_TRUE(h.valid());
        h.schedule();
    }

    while (executed.load() < N) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    laneThread.join();

    group.wait();
    EXPECT_EQ(executed.load(), N);
    EXPECT_EQ(wrongThread.load(), 0);  // every pinned contract ran on the lane's thread
}

TEST(ThreadPinning, WorkServiceWorkerDrainsItsOwnLane) {
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);

    // Lane count matches the worker count: lane == worker id
    WorkContractGroup group(256, "PinService", 4);
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    // Discover each worker's thread id by running unpinned probes
    std::mutex idMutex;
    std::map<size_t, std::thread::id> laneToThread;
    std::atomic<int> probes{0};
    for (int i = 0; i < 64; ++i) {
        auto h = group.createContract([&]() noexcept {
            {
                std::lock_guard<std::mutex> lock(idMutex);
                laneToThread[WorkService::getThreadId()] = std::this_thread::get_id();
            }
            probes.fetch_add(1);
        });
        ASSERT_TRUE(h.valid());
        h.schedule();
    }
    group.wait();
    ASSERT_EQ(probes.load(), 64);

    // Pin work to each observed lane and verify it executes on that lane's thread
    std::atomic<int> executed{0};
    std::atomic<int> wrongThread{0};
    int expected = 0;
    for (auto& [lane, threadId] : laneToThread) {
        for (int i = 0; i < 10; ++i) {
            auto h = group.createContract(
                [&, lane = lane]() noexcept {
                    std::thread::id expectedId;
                    {
                        std::lock_guard<std::mutex> lock(idMutex);
                        expectedId = laneToThread[lane];
                    }
                    if (std::this_thread::get_id() != expectedId) wrongThread.fetch_add(1);
                    executed.fetch_add(1);
                },
                ExecutionType::PinnedThread, static_cast<uint32_t>(lane));
            ASSERT_TRUE(h.valid());
            h.schedule();
            ++expected;
        }
    }

    group.wait();
    EXPECT_EQ(executed.load(), expected);
    EXPECT_EQ(wrongThread.load(), 0);

    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(ThreadPinning, WorkGraphNodesPinToLanes) {
    WorkService::Config config;
    config.threadCount = 2;
    WorkService service(config);

    WorkContractGroup group(64, "PinGraph", 2);
    ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);
    service.start();

    WorkGraph graph(&group);

    // Build-time validation: out-of-range lane throws
    EXPECT_THROW(graph.addNode([]() {}, "bad", nullptr, ExecutionType::PinnedThread, 7), std::invalid_argument);

    std::atomic<int> order{0};
    std::thread::id firstThread{};
    std::thread::id secondThread{};

    auto a = graph.addNode(
        [&]() {
            firstThread = std::this_thread::get_id();
            order.fetch_add(1);
        },
        "pinned-a", nullptr, ExecutionType::PinnedThread, 0);
    auto b = graph.addNode(
        [&]() {
            secondThread = std::this_thread::get_id();
            order.fetch_add(1);
        },
        "pinned-b", nullptr, ExecutionType::PinnedThread, 0);
    graph.addDependency(a, b);

    graph.execute();
    auto result = graph.wait();

    EXPECT_TRUE(result.allCompleted);
    EXPECT_EQ(order.load(), 2);
    // Both nodes pinned to lane 0 must have run on the same thread
    EXPECT_EQ(firstThread, secondThread);

    ASSERT_EQ(service.removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    service.stop();
}

TEST(ThreadPinning, MixedContention_CountersCoherent) {
    WorkContractGroup group(256, "PinContention", 2);
    std::atomic<int> executed{0};
    std::atomic<bool> stop{false};

    std::thread lane0([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            group.executePinnedWork(0);
            std::this_thread::yield();
        }
        group.executePinnedWork(0);
    });
    std::thread lane1([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            group.executePinnedWork(1);
            std::this_thread::yield();
        }
        group.executePinnedWork(1);
    });
    std::thread bg([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            group.executeAllBackgroundWork();
            std::this_thread::yield();
        }
        group.executeAllBackgroundWork();
    });

    constexpr int N = 3000;
    int created = 0;
    for (int i = 0; i < N;) {
        WorkContractHandle h;
        switch (i % 3) {
            case 0: h = group.createContract([&]() noexcept { executed.fetch_add(1); }); break;
            case 1: h = group.createContract([&]() noexcept { executed.fetch_add(1); }, ExecutionType::PinnedThread, 0); break;
            case 2: h = group.createContract([&]() noexcept { executed.fetch_add(1); }, ExecutionType::PinnedThread, 1); break;
        }
        if (!h.valid()) {
            std::this_thread::yield();
            continue;
        }
        h.schedule();
        ++created;
        ++i;
    }

    while (executed.load() < created) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    lane0.join();
    lane1.join();
    bg.join();

    group.wait();
    EXPECT_EQ(executed.load(), created);
    EXPECT_EQ(group.scheduledCount(), 0u);
    EXPECT_EQ(group.executingCount(), 0u);
    EXPECT_EQ(group.activeCount(), 0u);
}
