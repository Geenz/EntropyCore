#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkGraphTypes.h"
#include "Concurrency/WorkService.h"
#include "Core/EntropyApplication.h"

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

namespace
{
class CountingDelegate : public EntropyEngine::Core::EntropyAppDelegate
{
public:
    std::atomic<uint64_t> mainLoopCalls{0};
    std::promise<void> launched;

    void applicationDidFinishLaunching() override {
        launched.set_value();
    }
    void applicationMainLoop() override {
        mainLoopCalls.fetch_add(1, std::memory_order_relaxed);
    }
};
}  // namespace

TEST(MainThreadWork, ApplicationMainLoopWaitsForWork) {
    using Clock = std::chrono::steady_clock;
    auto& app = EntropyEngine::Core::EntropyApplication::shared();
    // Static: a run() that never returns keeps using it after the test body ends.
    static CountingDelegate delegate;

    EntropyEngine::Core::EntropyApplicationConfig cfg;
    cfg.workerThreads = 2;
    cfg.mainLoopWaitsForWork = true;
    app.configure(cfg);
    delegate.mainLoopCalls.store(0);
    delegate.launched = std::promise<void>();  // fresh per run (--gtest_repeat)
    app.setDelegate(&delegate);

    auto launchedFuture = delegate.launched.get_future();
    auto runResult = std::make_shared<std::promise<int>>();
    auto runFuture = runResult->get_future();
    std::thread runner([&app, runResult] { runResult->set_value(app.run()); });
    // Any early exit terminates the app and never blocks on a join.
    struct RunnerGuard
    {
        std::thread& thread;
        std::future<int>& result;
        ~RunnerGuard() {
            if (!thread.joinable()) return;
            EntropyEngine::Core::EntropyApplication::shared().terminate(0);
            if (result.wait_for(std::chrono::seconds(1)) == std::future_status::ready) {
                thread.join();
            } else {
                thread.detach();
            }
        }
    } runnerGuard{runner, runFuture};

    ASSERT_EQ(launchedFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const uint64_t before = delegate.mainLoopCalls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const uint64_t idleIterations = delegate.mainLoopCalls.load() - before;
    EXPECT_LE(idleIterations, 2u) << "main loop iterations over 300 ms idle";

    auto workService = app.services().get<WorkService>();
    ASSERT_TRUE(static_cast<bool>(workService));
    WorkContractGroup group(64, "AppMainLoopWaitTest");
    ASSERT_EQ(workService->addWorkContractGroup(&group), WorkService::GroupOperationStatus::Added);

    std::promise<void> ran;
    auto ranFuture = ran.get_future();
    auto h = group.createContract([&ran]() noexcept { ran.set_value(); }, ExecutionType::MainThread);
    ASSERT_TRUE(h.valid());
    h.schedule();

    EXPECT_EQ(ranFuture.wait_for(std::chrono::seconds(1)), std::future_status::ready)
        << "MainThread contract did not run on the waiting main loop";
    group.wait();
    ASSERT_EQ(workService->removeWorkContractGroup(&group), WorkService::GroupOperationStatus::Removed);
    workService = {};

    app.terminate(0);
    const bool returned = runFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    ASSERT_TRUE(returned) << "run() did not return within 5 s of terminate(0)";
    runner.join();
    EXPECT_EQ(runFuture.get(), 0);
    app.setDelegate(nullptr);
}
