#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "Concurrency/WorkContractGroup.h"

using namespace EntropyEngine::Core::Concurrency;

// Producers schedule while consumers drain; wait() must not return before every
// scheduled task has actually executed (regression: scheduled--/executing++
// ordering let wait() observe both counters at zero mid-handoff, and the
// bit-before-count publish order underflowed scheduledCount to SIZE_MAX).
TEST(WorkContractHighContention, ProducersAndConsumers_WaitCoversEverything) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 2000;

    WorkContractGroup group(1024, "ContentionTest");
    std::atomic<int> executed{0};
    std::atomic<int> created{0};
    std::atomic<bool> producersDone{false};

    std::vector<std::thread> threads;
    threads.reserve(kProducers + kConsumers);

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kPerProducer;) {
                auto h = group.createContract([&executed]() noexcept { executed.fetch_add(1); });
                if (!h.valid()) {
                    std::this_thread::yield();  // group full; consumers will drain
                    continue;
                }
                created.fetch_add(1);
                h.schedule();
                ++i;
            }
        });
    }

    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&]() {
            while (!producersDone.load(std::memory_order_acquire) || group.scheduledCount() > 0) {
                group.executeAllBackgroundWork();
                std::this_thread::yield();
            }
        });
    }

    for (int p = 0; p < kProducers; ++p) threads[p].join();
    producersDone.store(true, std::memory_order_release);
    for (int c = 0; c < kConsumers; ++c) threads[kProducers + c].join();

    group.wait();

    EXPECT_EQ(created.load(), kProducers * kPerProducer);
    EXPECT_EQ(executed.load(), created.load());
    EXPECT_EQ(group.scheduledCount(), 0u);
    EXPECT_EQ(group.executingCount(), 0u);
    EXPECT_EQ(group.activeCount(), 0u);
}

// Concurrent schedule/unschedule/release against a draining thread must never
// corrupt counters or strand contracts (regression: validate-then-CAS windows
// in releaseContract, and release racing the schedule publish).
TEST(WorkContractHighContention, ScheduleReleaseRaces_CountersStayCoherent) {
    constexpr int kIterations = 3000;

    WorkContractGroup group(256, "RaceTest");
    std::atomic<int> executed{0};
    std::atomic<bool> stopDrain{false};

    std::thread drainer([&]() {
        while (!stopDrain.load(std::memory_order_acquire)) {
            group.executeAllBackgroundWork();
            std::this_thread::yield();
        }
        group.executeAllBackgroundWork();
    });

    std::thread churner([&]() {
        for (int i = 0; i < kIterations; ++i) {
            auto h = group.createContract([&executed]() noexcept { executed.fetch_add(1); });
            if (!h.valid()) {
                std::this_thread::yield();
                continue;
            }
            h.schedule();
            if (i % 3 == 0) {
                h.unschedule();  // may lose to the drainer; both outcomes legal
                h.release();
            }
        }
    });

    churner.join();
    stopDrain.store(true, std::memory_order_release);
    drainer.join();

    group.wait();
    EXPECT_EQ(group.scheduledCount(), 0u);
    EXPECT_EQ(group.executingCount(), 0u);
    EXPECT_EQ(group.activeCount(), 0u);
    // Executed count is nondeterministic (releases race execution); coherence of
    // the terminal counters is the assertion.
}
