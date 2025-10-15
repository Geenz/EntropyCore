#include <gtest/gtest.h>
#include <atomic>
#include "Concurrency/WorkContractGroup.h"

using namespace EntropyEngine::Core::Concurrency;

TEST(WorkContractGroupAccounting, ScheduleAndExecute_AllCountersReturnToZero) {
    WorkContractGroup group(256, "AcctTest");
    std::atomic<int> executed{0};

    const int N = 50;
    for (int i = 0; i < N; ++i) {
        auto h = group.createContract([&executed]() noexcept { executed.fetch_add(1, std::memory_order_relaxed); });
        auto res = h.schedule();
        ASSERT_TRUE(res == ScheduleResult::Scheduled || res == ScheduleResult::AlreadyScheduled);
    }

    // Execute on calling thread deterministically
    group.executeAllBackgroundWork();

    // Ensure all background work is done
    group.wait();

    EXPECT_EQ(executed.load(), N);
    EXPECT_EQ(group.scheduledCount(), 0u);
    EXPECT_EQ(group.executingCount(), 0u);
    EXPECT_EQ(group.activeCount(), 0u);
}
