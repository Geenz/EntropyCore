/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Engine project.
 */

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <atomic>
#include "Core/TimerService.h"
#include "Concurrency/WorkService.h"

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace std::chrono_literals;

class TimerServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create WorkService
        WorkService::Config workConfig;
        workConfig.threadCount = 2;
        workService = std::make_shared<WorkService>(workConfig);

        // Create TimerService
        timerService = std::make_shared<TimerService>();

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
        // Stop services
        if (timerService) {
            timerService->stop();
            timerService->unload();
        }
        if (workService) {
            workService->stop();
            workService->unload();
        }

        timerService.reset();
        workService.reset();
    }

    std::shared_ptr<WorkService> workService;
    std::shared_ptr<TimerService> timerService;
};

TEST_F(TimerServiceTest, ServiceLifecycle) {
    // Verify services were created and initialized successfully
    EXPECT_NE(timerService, nullptr);
    EXPECT_NE(workService, nullptr);

    // Note: State management is the responsibility of ServiceRegistry, not the service itself.
    // When calling lifecycle methods directly (not through registry), state remains Registered.
    // This is expected behavior - services don't manage their own state transitions.
}

TEST_F(TimerServiceTest, OneShotTimer_Fires) {
    std::atomic<bool> fired{false};

    auto timer = timerService->scheduleTimer(
        50ms,
        [&fired]() { fired.store(true, std::memory_order_release); },
        false  // One-shot
    );

    EXPECT_TRUE(timer.isValid());
    EXPECT_FALSE(timer.isRepeating());

    // Wait for timer to fire
    std::this_thread::sleep_for(150ms);

    EXPECT_TRUE(fired.load(std::memory_order_acquire));
}

TEST_F(TimerServiceTest, OneShotTimer_DoesNotRepeat) {
    std::atomic<int> count{0};

    auto timer = timerService->scheduleTimer(
        50ms,
        [&count]() { count.fetch_add(1, std::memory_order_relaxed); },
        false  // One-shot
    );

    // Wait longer than one interval
    std::this_thread::sleep_for(200ms);

    // Should only fire once
    EXPECT_EQ(count.load(std::memory_order_acquire), 1);
}

TEST_F(TimerServiceTest, RepeatingTimer_FiresMultipleTimes) {
    std::atomic<int> count{0};

    auto timer = timerService->scheduleTimer(
        50ms,
        [&count]() { count.fetch_add(1, std::memory_order_relaxed); },
        true  // Repeating
    );

    EXPECT_TRUE(timer.isValid());
    EXPECT_TRUE(timer.isRepeating());
    EXPECT_EQ(timer.getInterval(), 50ms);

    // Wait for multiple intervals
    std::this_thread::sleep_for(250ms);

    // Should fire multiple times (at least 3-4 times)
    // Upper bound should be tight now that we fixed timer drift accumulation
    int finalCount = count.load(std::memory_order_acquire);
    EXPECT_GE(finalCount, 3);
    EXPECT_LE(finalCount, 6);  // 250ms / 50ms = 5 expected, allow ±1 for scheduling variance
}

TEST_F(TimerServiceTest, TimerCancellation_PreventsExecution) {
    std::atomic<bool> fired{false};

    auto timer = timerService->scheduleTimer(
        100ms,
        [&fired]() { fired.store(true, std::memory_order_release); },
        false  // One-shot
    );

    EXPECT_TRUE(timer.isValid());

    // Cancel immediately
    timer.invalidate();

    EXPECT_FALSE(timer.isValid());

    // Wait longer than the interval
    std::this_thread::sleep_for(200ms);

    // Should not have fired
    EXPECT_FALSE(fired.load(std::memory_order_acquire));
}

TEST_F(TimerServiceTest, RepeatingTimer_CancellationStopsFiring) {
    std::atomic<int> count{0};

    auto timer = timerService->scheduleTimer(
        50ms,
        [&count]() { count.fetch_add(1, std::memory_order_relaxed); },
        true  // Repeating
    );

    // Let it fire a few times
    std::this_thread::sleep_for(150ms);

    int countBeforeCancel = count.load(std::memory_order_acquire);
    EXPECT_GE(countBeforeCancel, 2);

    // Cancel the timer
    timer.invalidate();

    // Wait for more intervals
    std::this_thread::sleep_for(150ms);

    // Count should not increase significantly after cancellation
    int countAfterCancel = count.load(std::memory_order_acquire);
    EXPECT_LE(countAfterCancel, countBeforeCancel + 1);  // Allow for one in-flight execution
}

TEST_F(TimerServiceTest, MultipleTimers_ExecuteIndependently) {
    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    auto timer1 = timerService->scheduleTimer(
        50ms,
        [&count1]() { count1.fetch_add(1, std::memory_order_relaxed); },
        true  // Repeating
    );

    auto timer2 = timerService->scheduleTimer(
        75ms,
        [&count2]() { count2.fetch_add(1, std::memory_order_relaxed); },
        true  // Repeating
    );

    // Wait for timers to execute
    std::this_thread::sleep_for(350ms);

    // Both repeating timers should have fired multiple times
    EXPECT_GE(count1.load(std::memory_order_acquire), 4);  // At least 4 times at 50ms intervals
    EXPECT_GE(count2.load(std::memory_order_acquire), 3);  // At least 3 times at 75ms intervals
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
        ExecutionType::MainThread
    );

    // Pump main thread work for a while
    auto start = std::chrono::steady_clock::now();
    while (!fired.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() - start < 500ms) {
        workService->executeMainThreadWork(10);
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_TRUE(fired.load(std::memory_order_acquire));
    EXPECT_EQ(executionThreadId.load(std::memory_order_acquire), mainThreadId);
}

TEST_F(TimerServiceTest, TimerMove_TransfersOwnership) {
    std::atomic<int> count{0};

    auto timer1 = timerService->scheduleTimer(
        50ms,
        [&count]() { count.fetch_add(1, std::memory_order_relaxed); },
        true  // Repeating
    );

    EXPECT_TRUE(timer1.isValid());

    // Move to timer2
    Timer timer2 = std::move(timer1);

    EXPECT_FALSE(timer1.isValid());  // timer1 should be invalidated
    EXPECT_TRUE(timer2.isValid());   // timer2 should be valid

    // Wait for timer to fire
    std::this_thread::sleep_for(150ms);

    EXPECT_GE(count.load(std::memory_order_acquire), 2);

    // Cancel timer2
    timer2.invalidate();

    EXPECT_FALSE(timer2.isValid());
}

TEST_F(TimerServiceTest, TimerDestruction_CancelsTimer) {
    std::atomic<int> count{0};

    {
        auto timer = timerService->scheduleTimer(
            50ms,
            [&count]() { count.fetch_add(1, std::memory_order_relaxed); },
            true  // Repeating
        );

        // Let it fire once or twice
        std::this_thread::sleep_for(100ms);
    }  // timer goes out of scope

    int countAtDestruction = count.load(std::memory_order_acquire);

    // Wait longer
    std::this_thread::sleep_for(150ms);

    // Count should not increase significantly after timer destruction
    int countAfterDestruction = count.load(std::memory_order_acquire);
    EXPECT_LE(countAfterDestruction, countAtDestruction + 1);
}

TEST_F(TimerServiceTest, VeryShortInterval_StillWorks) {
    std::atomic<int> count{0};

    auto timer = timerService->scheduleTimer(
        1ms,  // Very short interval
        [&count]() { count.fetch_add(1, std::memory_order_relaxed); },
        true  // Repeating
    );

    // Wait
    std::this_thread::sleep_for(100ms);

    // Should have fired many times
    EXPECT_GE(count.load(std::memory_order_acquire), 10);
}
