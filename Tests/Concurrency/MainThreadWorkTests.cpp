#include <gtest/gtest.h>

#include <atomic>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkGraphTypes.h"

using namespace EntropyEngine::Core::Concurrency;

TEST(MainThreadWork, ScheduleAndDrain_MainThreadTasks) {
    WorkContractGroup group(128, "MTTest");

    std::atomic<int> ran{0};
    const int N = 7;

    for (int i = 0; i < N; ++i) {
        auto h = group.createContract([&ran]() noexcept { ran.fetch_add(1, std::memory_order_relaxed); },
                                      ExecutionType::MainThread);
        auto res = h.schedule();
        ASSERT_TRUE(res == ScheduleResult::Scheduled || res == ScheduleResult::AlreadyScheduled);
    }

    // Drain all main-thread work in the calling thread
    size_t executed = group.executeAllMainThreadWork();

    EXPECT_EQ(static_cast<int>(executed), N);
    EXPECT_EQ(ran.load(), N);
    EXPECT_EQ(group.mainThreadScheduledCount(), 0u);
    EXPECT_EQ(group.mainThreadExecutingCount(), 0u);
}
