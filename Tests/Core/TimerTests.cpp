/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Engine project.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "Concurrency/WorkService.h"
#include "Core/RefObject.h"
#include "Core/TimerService.h"
#include "ThreadCpuHelpers.h"

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace std::chrono_literals;

// Platform-specific drain times for timer test cleanup
// Windows requires longer drain times due to different thread scheduling characteristics
// that delay callback completion. Unix systems complete callbacks faster.
#ifdef _WIN32
constexpr auto kWindowsDrainTime = 200ms;  // Windows: slower callback completion
#else
constexpr auto kUnixDrainTime = 50ms;  // Unix (macOS, Linux): faster callback completion
#endif

class TimerServiceTest : public ::testing::Test
{
protected:
    void SetUp() override {
        // Create WorkService
        WorkService::Config workConfig;
        workConfig.threadCount = 2;
        workService = makeRef<WorkService>(workConfig);

        // Create TimerService
        timerService = makeRef<TimerService>();

        // Load services
        workService->load();
        timerService->load();

        // Inject WorkService into TimerService
        timerService->setWorkService(workService.get());

        // Start services
        workService->start();
        timerService->start();
    }

    void TearDown() override {
        // NOTE: Timer tests are timing-sensitive and may exhibit platform-specific behavior
        // in resource-constrained CI environments due to thread scheduling variability.
        // Windows requires longer drain times (200ms) vs Unix (50ms) for reliable cleanup.

        try {
            // Stop services
            if (timerService) {
                timerService->stop();
                timerService->unload();
            }
            if (workService) {
                workService->stop();
                workService->unload();
            }

            // CRITICAL: Extra safety drain before destroying services
            //
            // CI environments are often resource-constrained with:
            // - High CPU contention (multiple builds running in parallel)
            // - Slower/virtualized hardware
            // - Variable scheduling latency
            // - Shared system resources
            //
            // This causes timing-related tests to be "wonky" because:
            // 1. Thread scheduling is less predictable
            // 2. Small sleep durations may wake much later than requested
            // 3. Work that should complete in 10ms might take 50ms+
            // 4. Race conditions that rarely manifest locally become common
            //
            // This drain period gives lingering timer callbacks time to complete
            // before we destroy the test fixture, preventing use-after-free crashes.
            // Must pump main thread work to ensure callbacks execute properly.
            //
            // Use platform-specific drain time constants defined at file scope
#ifdef _WIN32
            constexpr auto drainTime = kWindowsDrainTime;
#else
            constexpr auto drainTime = kUnixDrainTime;
#endif
            auto drainStart = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - drainStart < drainTime) {
                workService->executeMainThreadWork(10);
                std::this_thread::sleep_for(5ms);
            }

            timerService.reset();
            workService.reset();
        } catch (const std::exception& e) {
            // Log timing-sensitive test cleanup failure
            // These tests may fail in CI environments due to thread scheduling variability
            ADD_FAILURE() << "Timer test TearDown failed (timing-sensitive test on CI): " << e.what();

            // Attempt cleanup anyway to prevent resource leaks
            try {
                timerService.reset();
                workService.reset();
            } catch (...) {
                // Suppress secondary exceptions during cleanup
            }
        } catch (...) {
            ADD_FAILURE() << "Timer test TearDown failed with unknown exception (timing-sensitive test on CI)";

            // Attempt cleanup anyway
            try {
                timerService.reset();
                workService.reset();
            } catch (...) {
                // Suppress secondary exceptions during cleanup
            }
        }
    }

    RefObject<WorkService> workService;
    RefObject<TimerService> timerService;
};

TEST_F(TimerServiceTest, ServiceLifecycle) {
    // Verify services were created and initialized successfully
    EXPECT_TRUE(static_cast<bool>(timerService));
    EXPECT_TRUE(static_cast<bool>(workService));

    // Note: State management is the responsibility of ServiceRegistry, not the service itself.
    // When calling lifecycle methods directly (not through registry), state remains Registered.
    // This is expected behavior - services don't manage their own state transitions.
}

TEST_F(TimerServiceTest, OneShotTimer_Fires) {
    std::atomic<bool> fired{false};

    auto timer = timerService->scheduleTimer(
        50ms, [&fired]() { fired.store(true, std::memory_order_release); },
        false  // One-shot
    );

    EXPECT_TRUE(timer.isValid());
    EXPECT_FALSE(timer.isRepeating());

    // Wait for timer to fire - automatic pumping via main thread contract
    auto start = std::chrono::steady_clock::now();
    while (!fired.load(std::memory_order_acquire) && std::chrono::steady_clock::now() - start < 500ms) {
        // Must pump main thread work for automatic timer pumping
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_TRUE(fired.load(std::memory_order_acquire));

    // Cancel timer and wait for in-flight executions before 'fired' is destroyed
    timer.invalidate();
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 50ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(1ms);
    }
}

// TIMING-SENSITIVE: This test may be flaky on CI due to thread scheduling variability
TEST_F(TimerServiceTest, OneShotTimer_DoesNotRepeat) {
    // Use shared_ptr to ensure count outlives any callbacks
    auto count = std::make_shared<std::atomic<int>>(0);

    auto timer = timerService->scheduleTimer(
        50ms, [count]() { count->fetch_add(1, std::memory_order_relaxed); },  // Capture by value
        false                                                                 // One-shot
    );

    // Wait longer than one interval - automatic pumping via main thread work
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 200ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }

    // Should only fire once
    EXPECT_EQ(count->load(std::memory_order_acquire), 1);

    // Cancel timer and wait for in-flight executions
    timer.invalidate();
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 50ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(1ms);
    }
    // count's shared_ptr keeps it alive until all callbacks complete
}

// TIMING-SENSITIVE: This test may be flaky on CI due to thread scheduling variability
TEST_F(TimerServiceTest, RepeatingTimer_FiresMultipleTimes) {
    // Use shared_ptr to ensure count outlives any callbacks
    auto count = std::make_shared<std::atomic<int>>(0);

    auto timer = timerService->scheduleTimer(
        50ms, [count]() { count->fetch_add(1, std::memory_order_relaxed); },  // Capture by value
        true                                                                  // Repeating
    );

    EXPECT_TRUE(timer.isValid());
    EXPECT_TRUE(timer.isRepeating());
    EXPECT_EQ(timer.getInterval(), 50ms);

    // Wait for multiple intervals - automatic pumping via main thread work
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 250ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }

    // Should fire multiple times (at least 3-4 times)
    // Upper bound should be tight now that we fixed timer drift accumulation
    int finalCount = count->load(std::memory_order_acquire);
    EXPECT_GE(finalCount, 3);
    EXPECT_LE(finalCount, 6);  // 250ms / 50ms = 5 expected, allow ±1 for scheduling variance

    // Cancel timer and wait for in-flight executions
    timer.invalidate();
    auto cancelStart = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - cancelStart < 50ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }
    // count's shared_ptr keeps it alive until all callbacks complete
}

// TIMING-SENSITIVE: This test may be flaky on CI due to thread scheduling variability
TEST_F(TimerServiceTest, TimerCancellation_PreventsExecution) {
    // Use shared_ptr to ensure fired outlives any callbacks
    auto fired = std::make_shared<std::atomic<bool>>(false);

    auto timer = timerService->scheduleTimer(
        100ms, [fired]() { fired->store(true, std::memory_order_release); },  // Capture by value
        false                                                                 // One-shot
    );

    EXPECT_TRUE(timer.isValid());

    // Cancel immediately
    timer.invalidate();

    EXPECT_FALSE(timer.isValid());

    // Wait longer than the interval
    std::this_thread::sleep_for(200ms);

    // Should not have fired
    EXPECT_FALSE(fired->load(std::memory_order_acquire));
    // fired's shared_ptr keeps it alive until all callbacks complete
}

// TIMING-SENSITIVE: This test may be flaky on CI due to thread scheduling variability
TEST_F(TimerServiceTest, RepeatingTimer_CancellationStopsFiring) {
    // Use shared_ptr to ensure count outlives any callbacks
    auto count = std::make_shared<std::atomic<int>>(0);

    auto timer = timerService->scheduleTimer(
        50ms, [count]() { count->fetch_add(1, std::memory_order_relaxed); },  // Capture by value
        true                                                                  // Repeating
    );

    // Let it fire a few times - automatic pumping via main thread work
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 150ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }

    int countBeforeCancel = count->load(std::memory_order_acquire);
    EXPECT_GE(countBeforeCancel, 2);

    // Cancel the timer
    timer.invalidate();

    // Wait for more intervals
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 150ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }

    // Count should not increase significantly after cancellation
    int countAfterCancel = count->load(std::memory_order_acquire);
    EXPECT_LE(countAfterCancel, countBeforeCancel + 1);  // Allow for one in-flight execution
    // count's shared_ptr keeps it alive until all callbacks complete
}

TEST_F(TimerServiceTest, MultipleTimers_ExecuteIndependently) {
    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    auto timer1 = timerService->scheduleTimer(
        50ms, [&count1]() { count1.fetch_add(1, std::memory_order_relaxed); },
        true  // Repeating
    );

    auto timer2 = timerService->scheduleTimer(
        75ms, [&count2]() { count2.fetch_add(1, std::memory_order_relaxed); },
        true  // Repeating
    );

    // Wait for timers to execute - automatic pumping via main thread work
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 350ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }

    // Both repeating timers should have fired multiple times
    EXPECT_GE(count1.load(std::memory_order_acquire), 4);  // At least 4 times at 50ms intervals
    EXPECT_GE(count2.load(std::memory_order_acquire), 3);  // At least 3 times at 75ms intervals

    // CRITICAL: Cancel timers and wait for in-flight executions to complete
    // before local atomics (count1, count2) go out of scope
    timer1.invalidate();
    timer2.invalidate();

    // Give any in-flight timer callbacks time to complete
    // This prevents use-after-free when count1/count2 are destroyed
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 50ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }
}

TEST_F(TimerServiceTest, MainThreadTimer_ExecutesOnMainThread) {
    std::atomic<bool> fired{false};
    std::thread::id mainThreadId = std::this_thread::get_id();
    std::atomic<std::thread::id> executionThreadId;

    auto timer = timerService->scheduleTimer(
        50ms,
        [&fired, &executionThreadId]() {
            executionThreadId.store(std::this_thread::get_id(), std::memory_order_release);
            fired.store(true, std::memory_order_release);
        },
        false,  // One-shot
        ExecutionType::MainThread);

    // Pump main thread work for automatic timer pumping
    auto start = std::chrono::steady_clock::now();
    while (!fired.load(std::memory_order_acquire) && std::chrono::steady_clock::now() - start < 500ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_TRUE(fired.load(std::memory_order_acquire));
    EXPECT_EQ(executionThreadId.load(std::memory_order_acquire), mainThreadId);

    // Cancel timer and wait for in-flight executions before locals are destroyed
    timer.invalidate();
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 50ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(1ms);
    }
}

// TIMING-SENSITIVE: This test may be flaky on CI due to thread scheduling variability
TEST_F(TimerServiceTest, TimerMove_TransfersOwnership) {
    // Use shared_ptr to ensure count outlives any callbacks
    auto count = std::make_shared<std::atomic<int>>(0);

    auto timer1 = timerService->scheduleTimer(
        50ms, [count]() { count->fetch_add(1, std::memory_order_relaxed); },  // Capture by value
        true                                                                  // Repeating
    );

    EXPECT_TRUE(timer1.isValid());

    // Move to timer2
    Timer timer2 = std::move(timer1);

    EXPECT_FALSE(timer1.isValid());  // timer1 should be invalidated
    EXPECT_TRUE(timer2.isValid());   // timer2 should be valid

    // Wait for timer to fire - automatic pumping via main thread work
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 150ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_GE(count->load(std::memory_order_acquire), 2);

    // Cancel timer2
    timer2.invalidate();

    EXPECT_FALSE(timer2.isValid());

    // Wait for in-flight executions
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 50ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(1ms);
    }
    // count's shared_ptr keeps it alive until all callbacks complete
}

// TIMING-SENSITIVE: This test may be flaky on CI due to thread scheduling variability
TEST_F(TimerServiceTest, TimerDestruction_CancelsTimer) {
    // Use shared_ptr to ensure count outlives any callbacks
    auto count = std::make_shared<std::atomic<int>>(0);

    {
        auto timer = timerService->scheduleTimer(
            50ms, [count]() { count->fetch_add(1, std::memory_order_relaxed); },  // Capture by value
            true                                                                  // Repeating
        );

        // Let it fire once or twice
        std::this_thread::sleep_for(100ms);
    }  // timer goes out of scope

    int countAtDestruction = count->load(std::memory_order_acquire);

    // Wait longer
    std::this_thread::sleep_for(150ms);

    // Count should not increase significantly after timer destruction
    int countAfterDestruction = count->load(std::memory_order_acquire);
    EXPECT_LE(countAfterDestruction, countAtDestruction + 1);
    // count's shared_ptr keeps it alive until all callbacks complete
}

TEST_F(TimerServiceTest, VeryShortInterval_StillWorks) {
    std::atomic<int> count{0};

    auto timer = timerService->scheduleTimer(
        1ms,  // Very short interval
        [&count]() { count.fetch_add(1, std::memory_order_relaxed); },
        true  // Repeating
    );

    // Wait - automatic pumping via main thread work (frequent for short intervals)
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 100ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(1ms);  // Pump more frequently for short intervals
    }

    // Should have fired many times
    EXPECT_GE(count.load(std::memory_order_acquire), 10);

    // Cancel timer and wait for in-flight executions before 'count' is destroyed
    timer.invalidate();
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 50ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(1ms);
    }
}

TEST_F(TimerServiceTest, IdleTimerServiceDoesNotSpin) {
    // Worker tids are captured before the timer is armed so the barrier contracts do not overlap the measurement.
    std::vector<TestSupport::ThreadId> tids;
#if defined(__linux__)
    WorkContractGroup captureGroup(64, "TimerIdleTidCapture");
    ASSERT_EQ(workService->addWorkContractGroup(&captureGroup), WorkService::GroupOperationStatus::Added);
    tids = TestSupport::captureWorkerTids(*workService, captureGroup);
    ASSERT_EQ(workService->removeWorkContractGroup(&captureGroup), WorkService::GroupOperationStatus::Removed);
    ASSERT_EQ(tids.size(), workService->getThreadCount());
#endif

    auto timer = timerService->scheduleTimer(30s, []() {}, true);
    EXPECT_TRUE(timer.isValid());

    std::this_thread::sleep_for(200ms);  // settle

    const auto before = TestSupport::poolCpuTime(tids);
    std::this_thread::sleep_for(500ms);
    const auto after = TestSupport::poolCpuTime(tids);
    ASSERT_GE(before.count(), 0);
    ASSERT_GE(after.count(), 0);
    const double used = std::chrono::duration<double, std::milli>(after - before).count();

    EXPECT_LT(used, 25.0) << "idle pool used " << used << " ms CPU over 500 ms with one armed 30 s timer";

    timer.invalidate();
}

TEST_F(TimerServiceTest, TimerFiresWithoutPumpOnIdlePool) {
    auto oneShotFiredAt = std::make_shared<std::atomic<int64_t>>(-1);
    auto repeatCount = std::make_shared<std::atomic<int>>(0);
    const auto t0 = std::chrono::steady_clock::now();

    auto oneShot = timerService->scheduleTimer(
        50ms,
        [oneShotFiredAt, t0]() {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
            oneShotFiredAt->store(elapsed.count(), std::memory_order_release);
        },
        false);
    auto repeating = timerService->scheduleTimer(
        10ms, [repeatCount]() { repeatCount->fetch_add(1, std::memory_order_relaxed); }, true);

    // No executeMainThreadWork(): AnyThread timers fire on the idle pool.
    const auto bound = std::chrono::steady_clock::now() + 5s;
    while ((oneShotFiredAt->load(std::memory_order_acquire) < 0 || repeatCount->load(std::memory_order_acquire) < 2) &&
           std::chrono::steady_clock::now() < bound) {
        std::this_thread::sleep_for(1ms);
    }

    EXPECT_GE(oneShotFiredAt->load(std::memory_order_acquire), 50);  // fired, and never early
    EXPECT_GE(repeatCount->load(std::memory_order_acquire), 2);      // repeats without a pump

    oneShot.invalidate();
    repeating.invalidate();
    std::this_thread::sleep_for(50ms);  // drain in-flight bodies
}

TEST_F(TimerServiceTest, MainThreadTimerWakesWaitingMainThread) {
    std::atomic<bool> stopLoop{false};
    std::atomic<bool> fired{false};
    std::atomic<std::thread::id> firedOn{};
    std::atomic<std::thread::id> loopId{};

    std::thread loop([&]() {
        loopId.store(std::this_thread::get_id(), std::memory_order_release);
        while (!stopLoop.load(std::memory_order_acquire)) {
            const uint64_t snap = workService->mainThreadWorkSnapshot();
            const auto result = workService->executeMainThreadWork();
            if (stopLoop.load(std::memory_order_acquire)) break;
            workService->waitForMainThreadWork(snap, result.nextDue);
        }
    });

    // Make sure the loop is running before the timer is armed.
    while (loopId.load(std::memory_order_acquire) == std::thread::id{}) {
        std::this_thread::sleep_for(1ms);
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto timer = timerService->scheduleTimer(
        50ms,
        [&]() {
            firedOn.store(std::this_thread::get_id(), std::memory_order_release);
            fired.store(true, std::memory_order_release);
        },
        false, ExecutionType::MainThread);

    while (!fired.load(std::memory_order_acquire) && std::chrono::steady_clock::now() - t0 < 2s) {
        std::this_thread::sleep_for(1ms);
    }

    stopLoop.store(true, std::memory_order_release);
    workService->notifyMainThreadWorkAvailable();
    loop.join();

    EXPECT_TRUE(fired.load(std::memory_order_acquire));
    EXPECT_EQ(firedOn.load(std::memory_order_acquire), loopId.load(std::memory_order_acquire));
}


namespace
{
/// A WorkService and a TimerService built the way EntropyApplication builds them, with a custom timer config.
struct IsolatedTimerEnv
{
    explicit IsolatedTimerEnv(uint32_t workerCount, TimerService::Config timerConfig = {}) {
        WorkService::Config workConfig;
        workConfig.threadCount = workerCount;
        workService = makeRef<WorkService>(workConfig);
        timerService = makeRef<TimerService>(timerConfig);
        workService->load();
        timerService->load();
        workService->start();
    }

    void attach() {
        timerService->setWorkService(workService.get());
        timerService->start();
    }

    ~IsolatedTimerEnv() {
        timerService->stop();
        timerService->unload();
        workService->stop();
        workService->unload();
    }

    RefObject<WorkService> workService;
    RefObject<TimerService> timerService;
};
}  // namespace

TEST(TimerServiceIsolated, TimerServiceAddsNoThreads) {
#ifndef __linux__
    GTEST_SKIP() << "thread count is read from /proc/self/task";
#else
    IsolatedTimerEnv env(2);
    const size_t before = TestSupport::processThreadCount();

    env.attach();
    auto fired = std::make_shared<std::atomic<int>>(0);
    std::vector<Timer> timers;
    for (int i = 0; i < 10; ++i) {
        timers.push_back(env.timerService->scheduleTimer(
            5ms * (i + 1), [fired]() { fired->fetch_add(1, std::memory_order_relaxed); }, false));
    }

    const auto start = std::chrono::steady_clock::now();
    while (fired->load(std::memory_order_acquire) < 10 && std::chrono::steady_clock::now() - start < 2s) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(fired->load(std::memory_order_acquire), 10);
    EXPECT_EQ(TestSupport::processThreadCount(), before) << "TimerService must run on the WorkService's threads";
#endif
}

TEST(TimerServiceIsolated, CancelledTimerFreesItsSlot) {
    // One slot: a pending timer occupies exactly it, cancelling frees it well before the fire time.
    IsolatedTimerEnv env(2, TimerService::Config{1});
    env.attach();

    auto first = env.timerService->scheduleTimer(30s, []() {}, false);
    ASSERT_TRUE(first.isValid());

    auto refused = env.timerService->scheduleTimer(30s, []() {}, false);
    EXPECT_FALSE(refused.isValid()) << "an armed timer must hold exactly one slot";

    first.invalidate();

    auto second = env.timerService->scheduleTimer(30s, []() {}, false);
    EXPECT_TRUE(second.isValid()) << "cancelling must free the slot before the fire time";
}

TEST(TimerServiceIsolated, StopWithArmedTimerIsPrompt) {
    IsolatedTimerEnv env(2);
    env.attach();

    auto oneShot = env.timerService->scheduleTimer(30s, []() {}, false);
    auto repeating = env.timerService->scheduleTimer(30s, []() {}, true);
    ASSERT_TRUE(oneShot.isValid());
    ASSERT_TRUE(repeating.isValid());

    const auto start = std::chrono::steady_clock::now();
    env.timerService->stop();

    // The timers are 30 s away; stopping must not wait for them.
    EXPECT_LT(std::chrono::steady_clock::now() - start, 10s) << "stop() must not wait for armed fire times";
    EXPECT_EQ(env.timerService->getActiveTimerCount(), 0u);
}

TEST(TimerServiceIsolated, TimerNeverFiresEarly) {
    IsolatedTimerEnv env(2);
    env.attach();

    constexpr int TIMER_COUNT = 100;
    struct Probe
    {
        std::chrono::steady_clock::time_point due;
        std::atomic<int64_t> firedNs{0};
    };
    std::vector<std::unique_ptr<Probe>> probes;
    std::vector<Timer> timers;
    probes.reserve(TIMER_COUNT);
    timers.reserve(TIMER_COUNT);

    for (int i = 0; i < TIMER_COUNT; ++i) {
        const auto delay = std::chrono::milliseconds(1 + (i * 7) % 50);
        auto probe = std::make_unique<Probe>();
        Probe* raw = probe.get();
        raw->due = std::chrono::steady_clock::now() + delay;
        timers.push_back(env.timerService->scheduleTimer(
            delay,
            [raw]() {
                raw->firedNs.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                   std::memory_order_release);
            },
            false));
        probes.push_back(std::move(probe));
    }

    const auto start = std::chrono::steady_clock::now();
    auto allFired = [&]() {
        for (const auto& probe : probes) {
            if (probe->firedNs.load(std::memory_order_acquire) == 0) return false;
        }
        return true;
    };
    while (!allFired() && std::chrono::steady_clock::now() - start < 3s) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(allFired());

    for (const auto& probe : probes) {
        EXPECT_GE(probe->firedNs.load(std::memory_order_acquire), probe->due.time_since_epoch().count());
    }
}

TEST(TimerServiceIsolated, FullGroupReturnsInvalidTimer) {
    IsolatedTimerEnv env(2, TimerService::Config{2});
    env.attach();

    auto first = env.timerService->scheduleTimer(30s, []() {}, false);
    auto second = env.timerService->scheduleTimer(30s, []() {}, true);
    auto third = env.timerService->scheduleTimer(30s, []() {}, false);

    EXPECT_TRUE(first.isValid());
    EXPECT_TRUE(second.isValid());
    EXPECT_FALSE(third.isValid()) << "a full contract group must yield an invalid Timer";
    EXPECT_EQ(env.timerService->getActiveTimerCount(), 2u);
}

TEST(TimerServiceIsolated, ScheduleAfterStopThrows) {
    IsolatedTimerEnv env(2);
    env.attach();
    env.timerService->stop();
    EXPECT_THROW(env.timerService->scheduleTimer(10ms, []() {}, false), std::runtime_error)
        << "a stopped TimerService must not hand out timers that never fire";
}

TEST(TimerServiceIsolated, RepeatingTimerNeedsPositiveInterval) {
    IsolatedTimerEnv env(2);
    env.attach();
    EXPECT_FALSE(env.timerService->scheduleTimer(0ms, []() {}, true).isValid());
    EXPECT_FALSE(env.timerService->scheduleTimer(-5ms, []() {}, true).isValid());
    EXPECT_TRUE(env.timerService->scheduleTimer(0ms, []() {}, false).isValid()) << "a one-shot may fire at once";
}
