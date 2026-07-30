#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkService.h"

using namespace EntropyEngine::Core::Concurrency;

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
