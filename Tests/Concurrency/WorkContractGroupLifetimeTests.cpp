#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkService.h"

using namespace EntropyEngine::Core::Concurrency;

// Destroying a group that is registered with a running service must be safe:
// the destructor's stop/wait protocol plus the worker's claim-under-registry-
// lock discipline close the pointer-escape window (regression: workers held a
// raw group pointer between dropping the registry lock and entering
// selectForExecution, invisible to the destructor's quiescence counters).
TEST(WorkContractGroupLifetime, DestroyRegisteredGroupWhileServiceRuns) {
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    service.start();

    for (int round = 0; round < 20; ++round) {
        std::atomic<int> executed{0};
        {
            WorkContractGroup group(128, "EphemeralGroup");
            ASSERT_EQ(service.addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);

            for (int i = 0; i < 64; ++i) {
                auto h = group.createContract([&executed]() noexcept { executed.fetch_add(1); });
                if (h.valid()) h.schedule();
            }
            // Destroy the group with work potentially still in flight; the
            // destructor must drain and deregister without racing the workers.
        }
        // Counter must be stable after destruction (no lost writer into freed memory)
        int snapshot = executed.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        EXPECT_EQ(executed.load(), snapshot);
    }

    service.stop();
}

// removeWorkContractGroup followed by immediate destruction is the documented
// removal flow; workers must never touch the group after remove() returns.
TEST(WorkContractGroupLifetime, RemoveThenDestroy_WorkersQuiesced) {
    WorkService::Config config;
    config.threadCount = 4;
    WorkService service(config);
    service.start();

    for (int round = 0; round < 20; ++round) {
        std::atomic<int> executed{0};
        auto group = std::make_unique<WorkContractGroup>(128, "RemovableGroup");
        ASSERT_EQ(service.addWorkContractGroup(group.get()), WorkService::GroupOperationStatus::Added);

        for (int i = 0; i < 64; ++i) {
            auto h = group->createContract([&executed]() noexcept { executed.fetch_add(1); });
            if (h.valid()) h.schedule();
        }

        ASSERT_EQ(service.removeWorkContractGroup(group.get()), WorkService::GroupOperationStatus::Removed);
        group.reset();  // must be safe immediately after removal
    }

    service.stop();
}
