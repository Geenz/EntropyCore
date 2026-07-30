#include <gtest/gtest.h>

#include <atomic>

#include "Concurrency/WorkContractGroup.h"

using namespace EntropyEngine::Core::Concurrency;

// A contract that schedules another contract from inside its own execution.
// The slot is freed before the task runs specifically to allow this; the chain
// must run to completion and the counters must return to zero.
TEST(WorkContractGroupReentrance, ContractSchedulesContract) {
    WorkContractGroup group(64, "ReentranceTest");
    std::atomic<int> depth{0};

    std::function<void()> chain = [&]() {
        if (depth.fetch_add(1) < 9) {
            auto next = group.createContract([&]() { chain(); });
            ASSERT_TRUE(next.valid());
            next.schedule();
        }
    };

    auto h = group.createContract(chain);
    ASSERT_TRUE(h.valid());
    h.schedule();

    // Each pass may schedule one more link; drain until the chain stops growing
    while (group.scheduledCount() > 0) {
        group.executeAllBackgroundWork();
    }
    group.wait();

    EXPECT_EQ(depth.load(), 10);
    EXPECT_EQ(group.scheduledCount(), 0u);
    EXPECT_EQ(group.activeCount(), 0u);
}

// Re-entrant scheduling at full capacity: freeing the slot before execution
// means a full group can still make progress one contract at a time.
TEST(WorkContractGroupReentrance, ReentranceAtFullCapacity) {
    WorkContractGroup group(2, "TinyGroup");
    std::atomic<int> ran{0};

    auto filler = group.createContract([&]() {
        // While this runs its slot is free; scheduling from here must succeed
        auto inner = group.createContract([&]() { ran.fetch_add(1); });
        ASSERT_TRUE(inner.valid());
        inner.schedule();
        ran.fetch_add(1);
    });
    auto blocker = group.createContract([&]() { ran.fetch_add(1); });
    ASSERT_TRUE(filler.valid());
    ASSERT_TRUE(blocker.valid());
    filler.schedule();
    blocker.schedule();

    while (group.scheduledCount() > 0) {
        group.executeAllBackgroundWork();
    }
    group.wait();

    EXPECT_EQ(ran.load(), 3);
    EXPECT_EQ(group.activeCount(), 0u);
}
