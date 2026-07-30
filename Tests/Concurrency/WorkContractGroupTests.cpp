#include <gtest/gtest.h>

#include <atomic>
#include <vector>

#include "Concurrency/WorkContractGroup.h"

using namespace EntropyEngine::Core::Concurrency;

TEST(WorkContractGroup, HandleLifecycle_InvalidAfterExecution) {
    WorkContractGroup group(64, "LifecycleTest");
    std::atomic<int> ran{0};

    auto h = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    EXPECT_TRUE(h.valid());
    EXPECT_EQ(h.schedule(), ScheduleResult::Scheduled);
    EXPECT_TRUE(h.isScheduled());

    group.executeAllBackgroundWork();
    group.wait();

    EXPECT_EQ(ran.load(), 1);
    EXPECT_FALSE(h.valid());  // generation bumped when the slot was freed
    EXPECT_EQ(h.schedule(), ScheduleResult::Invalid);
}

TEST(WorkContractGroup, UnscheduleThenRelease_NeverExecutes) {
    WorkContractGroup group(64, "UnschedTest");
    std::atomic<int> ran{0};

    auto h = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    ASSERT_EQ(h.schedule(), ScheduleResult::Scheduled);
    EXPECT_EQ(h.unschedule(), ScheduleResult::NotScheduled);
    h.release();
    EXPECT_FALSE(h.valid());

    group.executeAllBackgroundWork();
    group.wait();

    EXPECT_EQ(ran.load(), 0);
    EXPECT_EQ(group.activeCount(), 0u);
    EXPECT_EQ(group.scheduledCount(), 0u);
}

TEST(WorkContractGroup, CapacityExhaustion_ReturnsInvalidHandle) {
    WorkContractGroup group(4, "CapacityTest");
    std::vector<WorkContractHandle> handles;
    for (int i = 0; i < 4; ++i) {
        auto h = group.createContract([]() noexcept {});
        ASSERT_TRUE(h.valid());
        handles.push_back(h);
    }
    auto overflow = group.createContract([]() noexcept {});
    EXPECT_FALSE(overflow.valid());

    for (auto& h : handles) h.release();
    EXPECT_EQ(group.activeCount(), 0u);

    // Capacity available again after release
    auto again = group.createContract([]() noexcept {});
    EXPECT_TRUE(again.valid());
    again.release();
}

TEST(WorkContractGroup, StopPreventsSelection_ResumeRestores) {
    WorkContractGroup group(64, "StopTest");
    std::atomic<int> ran{0};

    auto h = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    ASSERT_EQ(h.schedule(), ScheduleResult::Scheduled);

    group.stop();
    auto selected = group.selectForExecution();
    EXPECT_FALSE(selected.valid());
    EXPECT_EQ(ran.load(), 0);

    group.resume();
    group.executeAllBackgroundWork();
    group.wait();
    EXPECT_EQ(ran.load(), 1);
}

TEST(WorkContractGroup, MainThreadWork_SeparateQueue) {
    WorkContractGroup group(64, "MainThreadTest");
    std::atomic<int> mainRan{0};
    std::atomic<int> bgRan{0};

    auto hm = group.createContract([&mainRan]() noexcept { mainRan.fetch_add(1); }, ExecutionType::MainThread);
    auto hb = group.createContract([&bgRan]() noexcept { bgRan.fetch_add(1); }, ExecutionType::AnyThread);
    ASSERT_EQ(hm.schedule(), ScheduleResult::Scheduled);
    ASSERT_EQ(hb.schedule(), ScheduleResult::Scheduled);

    EXPECT_TRUE(group.hasMainThreadWork());

    // Background drain must not touch main-thread work
    group.executeAllBackgroundWork();
    EXPECT_EQ(bgRan.load(), 1);
    EXPECT_EQ(mainRan.load(), 0);
    EXPECT_TRUE(group.hasMainThreadWork());

    EXPECT_EQ(group.executeMainThreadWork(10), 1u);
    EXPECT_EQ(mainRan.load(), 1);
    EXPECT_FALSE(group.hasMainThreadWork());

    group.wait();
    EXPECT_EQ(group.activeCount(), 0u);
}
