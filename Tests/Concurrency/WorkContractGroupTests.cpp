#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <tuple>
#include <vector>

#include "Concurrency/IConcurrencyProvider.h"
#include "Concurrency/WorkContractGroup.h"

using namespace EntropyEngine::Core::Concurrency;
using namespace std::chrono_literals;
using SteadyClock = std::chrono::steady_clock;

namespace
{

/// Records every notification a group sends.
class RecordingProvider : public IConcurrencyProvider
{
public:
    void notifyWorkAvailable(WorkContractGroup*) override {
        std::lock_guard<std::mutex> lock(mutex);
        workNotifies.emplace_back(ExecutionType::AnyThread, 0u);
    }
    void notifyWorkAvailableFor(WorkContractGroup*, ExecutionType type, uint32_t lane) override {
        std::lock_guard<std::mutex> lock(mutex);
        workNotifies.emplace_back(type, lane);
    }
    void notifyMainThreadWorkAvailable(WorkContractGroup*) override {
        std::lock_guard<std::mutex> lock(mutex);
        ++mainThreadNotifies;
    }
    void notifyGroupDestroyed(WorkContractGroup*) override {}

    std::vector<std::pair<ExecutionType, uint32_t>> work() {
        std::lock_guard<std::mutex> lock(mutex);
        return workNotifies;
    }
    size_t mainThread() {
        std::lock_guard<std::mutex> lock(mutex);
        return mainThreadNotifies;
    }

private:
    std::mutex mutex;
    std::vector<std::pair<ExecutionType, uint32_t>> workNotifies;
    size_t mainThreadNotifies = 0;
};

}  // namespace

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

TEST(WorkContractGroup, TimedContract_SkippedUntilDue) {
    WorkContractGroup group(64, "TimedSkip");
    std::atomic<int> ran{0};

    auto h = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    const auto due = SteadyClock::now() + 50ms;
    ASSERT_EQ(h.scheduleAt(due), ScheduleResult::Scheduled);
    EXPECT_EQ(group.getContractState(h), ContractState::Scheduled);
    EXPECT_TRUE(h.isScheduled());
    EXPECT_EQ(group.scheduledCount(), 1u);
    EXPECT_EQ(group.timedCount(), 1u);

    // A pull before the due time skips the contract and reports its due time; the
    // contract stays scheduled and a second pull sees it again.
    auto nextDue = SteadyClock::time_point::max();
    EXPECT_FALSE(group.selectForExecution(std::nullopt, &nextDue).valid());
    EXPECT_EQ(nextDue, due);
    EXPECT_EQ(group.getContractState(h), ContractState::Scheduled);
    nextDue = SteadyClock::time_point::max();
    EXPECT_FALSE(group.selectForExecution(std::nullopt, &nextDue).valid());
    EXPECT_EQ(nextDue, due);
    group.executeAllBackgroundWork();
    EXPECT_EQ(ran.load(), 0);

    std::this_thread::sleep_until(due);
    group.executeAllBackgroundWork();
    group.wait();
    EXPECT_EQ(ran.load(), 1);
    EXPECT_EQ(group.timedCount(), 0u);
    EXPECT_EQ(group.activeCount(), 0u);
}

TEST(WorkContractGroup, TimedContract_PastDueSchedulesImmediately) {
    RecordingProvider provider;
    WorkContractGroup group(64, "TimedPast");
    group.setConcurrencyProvider(&provider);
    std::atomic<int> ran{0};

    auto a = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    ASSERT_EQ(a.scheduleAt(SteadyClock::now() - 1ms), ScheduleResult::Scheduled);
    EXPECT_EQ(group.getContractState(a), ContractState::Scheduled);

    auto b = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    ASSERT_EQ(b.scheduleAfter(SteadyClock::duration::zero()), ScheduleResult::Scheduled);
    EXPECT_EQ(group.getContractState(b), ContractState::Scheduled);

    EXPECT_EQ(group.scheduledCount(), 2u);
    EXPECT_EQ(group.timedCount(), 0u);
    EXPECT_EQ(provider.work().size(), 2u);

    group.executeAllBackgroundWork();
    EXPECT_EQ(ran.load(), 2);
    group.setConcurrencyProvider(nullptr);
}

TEST(WorkContractGroup, TimedContract_AlreadyScheduled) {
    WorkContractGroup group(64, "TimedAlready");
    auto h = group.createContract([]() noexcept {});
    ASSERT_EQ(h.scheduleAfter(1h), ScheduleResult::Scheduled);
    EXPECT_EQ(h.scheduleAfter(1h), ScheduleResult::AlreadyScheduled);
    EXPECT_EQ(h.schedule(), ScheduleResult::AlreadyScheduled);
    EXPECT_EQ(group.getContractState(h), ContractState::Scheduled);
    EXPECT_EQ(group.timedCount(), 1u);
    h.release();
    EXPECT_EQ(group.activeCount(), 0u);
    EXPECT_EQ(group.timedCount(), 0u);
}

TEST(WorkContractGroup, TimedContract_UnscheduleNeverRuns) {
    WorkContractGroup group(64, "TimedUnschedule");
    std::atomic<int> ran{0};

    auto h = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    const auto due = SteadyClock::now() + 50ms;
    ASSERT_EQ(h.scheduleAt(due), ScheduleResult::Scheduled);
    EXPECT_EQ(h.unschedule(), ScheduleResult::NotScheduled);
    EXPECT_EQ(group.getContractState(h), ContractState::Allocated);
    EXPECT_FALSE(h.isScheduled());
    EXPECT_EQ(group.timedCount(), 0u);
    EXPECT_EQ(group.scheduledCount(), 0u);
    EXPECT_EQ(h.unschedule(), ScheduleResult::NotScheduled);

    std::this_thread::sleep_until(due);
    group.executeAllBackgroundWork();
    EXPECT_EQ(ran.load(), 0);

    // An unscheduled timed contract can be scheduled again.
    const auto again = SteadyClock::now() + 20ms;
    ASSERT_EQ(h.scheduleAt(again), ScheduleResult::Scheduled);
    group.executeAllBackgroundWork();
    EXPECT_EQ(ran.load(), 0);
    std::this_thread::sleep_until(again);
    group.executeAllBackgroundWork();
    EXPECT_EQ(ran.load(), 1);
    EXPECT_EQ(group.activeCount(), 0u);
}

TEST(WorkContractGroup, TimedContract_ReleaseFreesSlot) {
    WorkContractGroup group(64, "TimedRelease");
    std::atomic<int> ran{0};

    auto h = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    const auto due = SteadyClock::now() + 50ms;
    ASSERT_EQ(h.scheduleAt(due), ScheduleResult::Scheduled);
    EXPECT_EQ(group.activeCount(), 1u);

    auto copy = h;
    h.release();
    EXPECT_FALSE(copy.valid());
    EXPECT_EQ(group.activeCount(), 0u);
    EXPECT_EQ(group.timedCount(), 0u);
    std::this_thread::sleep_until(due);
    group.executeAllBackgroundWork();
    EXPECT_EQ(ran.load(), 0);
    EXPECT_EQ(group.scheduledCount(), 0u);
}

TEST(WorkContractGroup, TimedContract_DestructorReleasesPending) {
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> watch = token;
    {
        WorkContractGroup group(64, "TimedDestroy");
        for (int i = 0; i < 3; ++i) {
            auto h = group.createContract([token]() noexcept { (void)token; });
            ASSERT_EQ(h.scheduleAfter(1h), ScheduleResult::Scheduled);
        }
        token.reset();
        EXPECT_FALSE(watch.expired());
        EXPECT_EQ(group.activeCount(), 3u);
    }
    EXPECT_TRUE(watch.expired());
}

TEST(WorkContractGroup, TimedContract_WaitWaitsForTimed) {
    WorkContractGroup group(64, "TimedWait");
    auto h = group.createContract([]() noexcept {});
    ASSERT_EQ(h.scheduleAfter(1h), ScheduleResult::Scheduled);
    auto waiter = std::async(std::launch::async, [&group] { group.wait(); });
    EXPECT_EQ(waiter.wait_for(100ms), std::future_status::timeout);
    h.release();
    EXPECT_EQ(waiter.wait_for(2s), std::future_status::ready);
}

TEST(WorkContractGroup, TimedContract_PullReportsEarliestSkippedDue) {
    RecordingProvider provider;
    WorkContractGroup group(64, "TimedNextDue");
    group.setConcurrencyProvider(&provider);
    std::atomic<int> ran{0};

    const auto base = SteadyClock::now() + 1s;
    auto c = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    auto a = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    auto b = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    ASSERT_EQ(c.scheduleAt(base + 300ms), ScheduleResult::Scheduled);
    ASSERT_EQ(a.scheduleAt(base + 100ms), ScheduleResult::Scheduled);
    ASSERT_EQ(b.scheduleAt(base + 200ms), ScheduleResult::Scheduled);

    // Scheduling notifies like schedule(): one wake per contract.
    EXPECT_EQ(provider.work().size(), 3u);
    EXPECT_EQ(group.timedCount(), 3u);

    auto nextDue = SteadyClock::time_point::max();
    EXPECT_FALSE(group.selectForExecution(std::nullopt, &nextDue).valid());
    EXPECT_EQ(nextDue, base + 100ms);
    EXPECT_EQ(group.scheduledCount(), 3u);

    EXPECT_EQ(a.unschedule(), ScheduleResult::NotScheduled);
    nextDue = SteadyClock::time_point::max();
    EXPECT_FALSE(group.selectForExecution(std::nullopt, &nextDue).valid());
    EXPECT_EQ(nextDue, base + 200ms);
    EXPECT_EQ(group.timedCount(), 2u);

    EXPECT_EQ(ran.load(), 0);
    a.release();
    b.release();
    c.release();
    EXPECT_EQ(group.activeCount(), 0u);
    EXPECT_EQ(group.timedCount(), 0u);
    group.setConcurrencyProvider(nullptr);
}

TEST(WorkContractGroup, TimedContract_ClaimWakesAnotherWorkerWhileTimedRemain) {
    RecordingProvider provider;
    WorkContractGroup group(64, "TimedChain");
    group.setConcurrencyProvider(&provider);
    std::atomic<int> ran{0};

    const auto due = SteadyClock::now() + 20ms;
    auto a = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    auto b = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    ASSERT_EQ(a.scheduleAt(due), ScheduleResult::Scheduled);
    ASSERT_EQ(b.scheduleAt(due), ScheduleResult::Scheduled);
    const size_t armNotifies = provider.work().size();

    std::this_thread::sleep_until(due);
    auto first = group.selectForExecution();
    ASSERT_TRUE(first.valid());
    EXPECT_EQ(provider.work().size(), armNotifies + 1);  // one more timed contract remains
    group.executeContract(first);

    auto second = group.selectForExecution();
    ASSERT_TRUE(second.valid());
    EXPECT_EQ(provider.work().size(), armNotifies + 1);  // none remains
    group.executeContract(second);

    EXPECT_EQ(ran.load(), 2);
    EXPECT_EQ(group.timedCount(), 0u);
    group.setConcurrencyProvider(nullptr);
}

TEST(WorkContractGroup, TimedContract_ExecutionTypesLandInOwnQueues) {
    RecordingProvider provider;
    WorkContractGroup group(64, "TimedTypes", 2);
    group.setConcurrencyProvider(&provider);
    std::atomic<int> anyRan{0};
    std::atomic<int> mainRan{0};
    std::atomic<int> pinnedRan{0};

    const auto due = SteadyClock::now() + 50ms;
    auto any = group.createContract([&anyRan]() noexcept { anyRan.fetch_add(1); });
    auto main = group.createContract([&mainRan]() noexcept { mainRan.fetch_add(1); }, ExecutionType::MainThread);
    auto pinned =
        group.createContract([&pinnedRan]() noexcept { pinnedRan.fetch_add(1); }, ExecutionType::PinnedThread, 1);
    ASSERT_EQ(any.scheduleAt(due), ScheduleResult::Scheduled);
    ASSERT_EQ(main.scheduleAt(due), ScheduleResult::Scheduled);
    ASSERT_EQ(pinned.scheduleAt(due), ScheduleResult::Scheduled);

    // Not due: each queue skips its contract.
    group.executeAllBackgroundWork();
    EXPECT_EQ(group.executePinnedWork(1), 0u);
    EXPECT_EQ(group.executeMainThreadWork(10), 0u);
    EXPECT_EQ(anyRan.load() + mainRan.load() + pinnedRan.load(), 0);

    EXPECT_FALSE(group.hasMainThreadWork());  // scheduled, but not due
    EXPECT_EQ(group.mainThreadScheduledCount(), 1u);
    EXPECT_TRUE(group.hasPinnedWork(1));
    EXPECT_FALSE(group.hasPinnedWork(0));
    EXPECT_EQ(group.scheduledCount(), 2u);  // AnyThread + PinnedThread

    EXPECT_EQ(provider.mainThread(), 1u);
    const auto work = provider.work();
    ASSERT_EQ(work.size(), 2u);
    int anyNotifies = 0;
    int pinnedNotifies = 0;
    for (const auto& [type, lane] : work) {
        if (type == ExecutionType::AnyThread) ++anyNotifies;
        if (type == ExecutionType::PinnedThread && lane == 1) ++pinnedNotifies;
    }
    EXPECT_EQ(anyNotifies, 1);
    EXPECT_EQ(pinnedNotifies, 1);

    std::this_thread::sleep_until(due);
    EXPECT_TRUE(group.hasMainThreadWork());  // due now
    group.executeAllBackgroundWork();
    EXPECT_EQ(anyRan.load(), 1);
    EXPECT_EQ(pinnedRan.load(), 0);
    EXPECT_EQ(mainRan.load(), 0);
    EXPECT_EQ(group.executePinnedWork(1), 1u);
    EXPECT_EQ(pinnedRan.load(), 1);
    EXPECT_EQ(group.executeMainThreadWork(10), 1u);
    EXPECT_EQ(mainRan.load(), 1);
    EXPECT_EQ(group.activeCount(), 0u);
    group.setConcurrencyProvider(nullptr);
}

TEST(WorkContractGroup, CheckTimedDeferrals_RunsCallbacksOnly) {
    WorkContractGroup group(64, "TimedCheck");
    std::atomic<int> callbackCalls{0};
    auto cb = group.addTimedDeferralCallback([&callbackCalls]() -> size_t {
        callbackCalls.fetch_add(1);
        return 2;
    });
    std::atomic<int> ran{0};
    auto h = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
    ASSERT_EQ(h.scheduleAfter(5ms), ScheduleResult::Scheduled);
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(group.checkTimedDeferrals(), 2u);
    EXPECT_EQ(callbackCalls.load(), 1);
    group.executeAllBackgroundWork();
    EXPECT_EQ(ran.load(), 1);
    group.removeTimedDeferralCallback(cb);
}

TEST(WorkContractGroup, TimedContract_DebugStringReportsTimed) {
    WorkContractGroup group(64, "TimedDebug");
    auto h = group.createContract([]() noexcept {});
    ASSERT_EQ(h.scheduleAfter(1h), ScheduleResult::Scheduled);
    EXPECT_NE(group.debugString().find("timed:1"), std::string::npos) << group.debugString();
    h.release();
    EXPECT_NE(group.debugString().find("timed:0"), std::string::npos) << group.debugString();
}

TEST(WorkContractGroup, TimedContract_StressScheduleUnscheduleRun) {
    constexpr size_t CAPACITY = 1024;
    constexpr int THREADS = 4;
    WorkContractGroup group(CAPACITY, "TimedStress");

    struct Record
    {
        std::atomic<int> runs{0};
        std::atomic<bool> cancelled{false};
        std::atomic<bool> early{false};
        std::atomic<int64_t> dueTicks{0};
    };
    std::mutex recordsMutex;
    std::deque<Record> records;
    auto newRecord = [&]() -> Record* {
        std::lock_guard<std::mutex> lock(recordsMutex);
        return &records.emplace_back();
    };

    std::atomic<size_t> armed{0};
    std::atomic<size_t> unscheduled{0};
    std::atomic<size_t> rearmed{0};
    std::atomic<size_t> scheduleFailures{0};
    const auto deadline = SteadyClock::now() + 1s;

    auto worker = [&](unsigned seed) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> delayUs(0, 3000);
        std::uniform_int_distribution<int> coin(0, 7);
        std::vector<std::pair<WorkContractHandle, Record*>> pending;

        auto arm = [&](WorkContractHandle& h, Record* rec) {
            const auto due = SteadyClock::now() + std::chrono::microseconds(delayUs(rng));
            rec->dueTicks.store(due.time_since_epoch().count(), std::memory_order_relaxed);
            ScheduleResult result = h.scheduleAt(due);
            // An unschedule that raced a peek is finished by the peeking worker; retry until it has.
            while (result == ScheduleResult::TryAgainLater) {
                std::this_thread::yield();
                result = h.scheduleAt(due);
            }
            if (result != ScheduleResult::Scheduled) scheduleFailures.fetch_add(1);
        };

        constexpr size_t MAX_ARMS_PER_THREAD = 50000;
        size_t arms = 0;
        Record* spare = nullptr;
        while (SteadyClock::now() < deadline) {
            if (arms >= MAX_ARMS_PER_THREAD) {
                group.executeAllBackgroundWork();
                continue;
            }
            Record* rec = spare ? spare : newRecord();
            spare = nullptr;
            auto h = group.createContract([rec]() noexcept {
                const auto now = SteadyClock::now().time_since_epoch().count();
                if (now < rec->dueTicks.load(std::memory_order_relaxed)) rec->early.store(true);
                rec->runs.fetch_add(1);
            });
            if (!h.valid()) {
                spare = rec;  // group full; reuse the record
                group.executeAllBackgroundWork();
                continue;
            }
            ++arms;
            arm(h, rec);
            armed.fetch_add(1);
            pending.emplace_back(h, rec);
            if (pending.size() > 64) pending.erase(pending.begin());

            if (coin(rng) < 2 && !pending.empty()) {
                auto& [victim, victimRec] = pending[static_cast<size_t>(rng()) % pending.size()];
                if (victim.unschedule() == ScheduleResult::NotScheduled) {
                    if (coin(rng) < 4) {
                        arm(victim, victimRec);
                        rearmed.fetch_add(1);
                    } else {
                        victimRec->cancelled.store(true);
                        victim.release();
                        unscheduled.fetch_add(1);
                    }
                }
            }

            if (coin(rng) < 4) group.executeAllBackgroundWork();
        }
        if (spare) spare->cancelled.store(true);  // never armed
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) threads.emplace_back(worker, 1234u + static_cast<unsigned>(t));
    for (auto& th : threads) th.join();

    const auto drainLimit = SteadyClock::now() + 5s;
    while (group.scheduledCount() > 0 && SteadyClock::now() < drainLimit) {
        group.executeAllBackgroundWork();
        std::this_thread::yield();
    }

    EXPECT_EQ(scheduleFailures.load(), 0u);
    EXPECT_GT(armed.load(), 1000u);
    EXPECT_GT(unscheduled.load(), 0u);
    EXPECT_GT(rearmed.load(), 0u);

    size_t ranOnce = 0;
    size_t wrong = 0;
    size_t early = 0;
    for (const auto& rec : records) {
        const int runs = rec.runs.load();
        if (rec.cancelled.load()) {
            if (runs != 0) ++wrong;
        } else {
            if (runs != 1) ++wrong;
            if (runs == 1) ++ranOnce;
        }
        if (rec.early.load()) ++early;
    }
    EXPECT_EQ(wrong, 0u);
    EXPECT_EQ(early, 0u);
    EXPECT_GT(ranOnce, 0u);

    EXPECT_EQ(group.timedCount(), 0u);
    EXPECT_EQ(group.activeCount(), 0u) << group.debugString();
    EXPECT_EQ(group.scheduledCount(), 0u);
    EXPECT_EQ(group.executingCount(), 0u);
    EXPECT_NE(group.debugString().find("timed:0"), std::string::npos) << group.debugString();
}

TEST(WorkContractGroup, Peek_PullOverNotDueLeavesQueueIntact) {
    // A pinned lane shows its queue through hasPinnedWork(): a pull that took a bit would clear it.
    WorkContractGroup group(64, "PeekIntact", 1);
    std::vector<WorkContractHandle> handles;
    for (int i = 0; i < 3; ++i) {
        auto h = group.createContract([]() noexcept {}, ExecutionType::PinnedThread, 0);
        ASSERT_EQ(h.scheduleAfter(1h), ScheduleResult::Scheduled);
        handles.push_back(h);
    }
    EXPECT_FALSE(group.selectForPinnedExecution(0).valid());
    EXPECT_TRUE(group.hasPinnedWork(0));
    EXPECT_EQ(group.executePinnedWork(0), 0u);
    EXPECT_TRUE(group.hasPinnedWork(0));
    EXPECT_EQ(group.scheduledCount(), 3u);
    for (auto& h : handles) {
        EXPECT_EQ(group.getContractState(h), ContractState::Scheduled);
        h.release();
    }
    EXPECT_FALSE(group.hasPinnedWork(0));
}

TEST(WorkContractGroup, Peek_StressUnscheduleAndReleaseRacePulls) {
    // Owners cancel contracts while pullers peek them: every cancel must hold (the
    // contract never runs), every rescheduled contract runs once, no slot leaks.
    constexpr size_t CAPACITY = 256;
    WorkContractGroup group(CAPACITY, "PeekCancel");
    std::atomic<bool> stop{false};
    std::atomic<size_t> ranAfterCancel{0};
    std::atomic<size_t> ranTwice{0};

    struct Record
    {
        std::atomic<int> runs{0};
        std::atomic<bool> cancelled{false};
        std::atomic<bool> rescheduled{false};
    };
    std::deque<Record> records;
    std::mutex recordsMutex;

    std::vector<std::thread> pullers;
    for (int t = 0; t < 3; ++t) {
        pullers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                group.executeAllBackgroundWork();
            }
        });
    }

    std::vector<std::thread> owners;
    for (int t = 0; t < 2; ++t) {
        owners.emplace_back([&, t] {
            std::mt19937 rng(7u + static_cast<unsigned>(t));
            std::uniform_int_distribution<int> coin(0, 3);
            const auto end = SteadyClock::now() + 500ms;
            while (SteadyClock::now() < end) {
                Record* rec;
                {
                    std::lock_guard<std::mutex> lock(recordsMutex);
                    rec = &records.emplace_back();
                }
                auto h = group.createContract([rec, &ranAfterCancel, &ranTwice]() noexcept {
                    if (rec->cancelled.load()) ranAfterCancel.fetch_add(1);
                    if (rec->runs.fetch_add(1) != 0) ranTwice.fetch_add(1);
                });
                if (!h.valid()) {
                    std::this_thread::yield();
                    continue;
                }
                // Due soon, so pullers are peeking it while the owner cancels.
                EXPECT_EQ(h.scheduleAfter(50us), ScheduleResult::Scheduled);
                if (h.unschedule() != ScheduleResult::NotScheduled) {
                    continue;  // already running or ran
                }
                if (coin(rng) < 2) {
                    rec->cancelled.store(true);
                    h.release();
                } else {
                    ScheduleResult result = h.schedule();
                    while (result == ScheduleResult::TryAgainLater) {
                        std::this_thread::yield();
                        result = h.schedule();
                    }
                    EXPECT_EQ(result, ScheduleResult::Scheduled);
                    rec->rescheduled.store(true);
                }
            }
        });
    }
    for (auto& o : owners) o.join();

    const auto drainLimit = SteadyClock::now() + 5s;
    while (group.scheduledCount() > 0 && SteadyClock::now() < drainLimit) {
        std::this_thread::sleep_for(1ms);
    }
    stop.store(true, std::memory_order_release);
    for (auto& p : pullers) p.join();

    size_t cancelled = 0;
    size_t lost = 0;
    for (const auto& rec : records) {
        if (rec.cancelled.load()) ++cancelled;
        if (rec.rescheduled.load() && rec.runs.load() != 1) ++lost;
    }
    EXPECT_GT(cancelled, 0u);
    EXPECT_EQ(ranAfterCancel.load(), 0u);
    EXPECT_EQ(ranTwice.load(), 0u);
    EXPECT_EQ(lost, 0u);
    EXPECT_EQ(group.scheduledCount(), 0u);
    EXPECT_EQ(group.executingCount(), 0u);
    EXPECT_EQ(group.activeCount(), 0u) << group.debugString();
    EXPECT_EQ(group.timedCount(), 0u);
}

TEST(WorkContractGroup, Peek_OneDueContractIsClaimedExactlyOnce) {
    constexpr int ROUNDS = 200;
    for (int round = 0; round < ROUNDS; ++round) {
        WorkContractGroup group(128, "PeekOnce");
        std::atomic<int> ran{0};
        std::vector<WorkContractHandle> notDue;
        for (int i = 0; i < 50; ++i) {
            auto h = group.createContract([]() noexcept {});
            ASSERT_EQ(h.scheduleAfter(1h), ScheduleResult::Scheduled);
            notDue.push_back(h);
        }
        auto due = group.createContract([&ran]() noexcept { ran.fetch_add(1); });
        ASSERT_EQ(due.schedule(), ScheduleResult::Scheduled);

        std::atomic<bool> go{false};
        std::atomic<int> claims{0};
        auto pull = [&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            auto h = group.selectForExecution();
            if (h.valid()) {
                claims.fetch_add(1);
                group.executeContract(h);
            }
        };
        std::thread a(pull);
        std::thread b(pull);
        go.store(true, std::memory_order_release);
        a.join();
        b.join();

        ASSERT_EQ(claims.load(), 1) << "round " << round;
        ASSERT_EQ(ran.load(), 1) << "round " << round;
        EXPECT_EQ(group.scheduledCount(), 50u);
        for (auto& h : notDue) h.release();
    }
}

TEST(WorkContractGroup, Peek_StressPullWhileOwnersReleaseAndReschedule) {
    constexpr size_t CAPACITY = 256;
    WorkContractGroup group(CAPACITY, "PeekStress");
    std::atomic<bool> stop{false};
    std::atomic<size_t> early{0};
    std::atomic<size_t> twice{0};

    struct Record
    {
        std::atomic<int> runs{0};
        std::atomic<int64_t> dueTicks{0};
        std::atomic<bool> released{false};
    };
    std::deque<Record> records;
    std::mutex recordsMutex;

    std::vector<std::thread> pullers;
    for (int t = 0; t < 3; ++t) {
        pullers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                group.executeAllBackgroundWork();
            }
        });
    }

    std::vector<std::thread> owners;
    std::atomic<size_t> released{0};
    for (int t = 0; t < 2; ++t) {
        owners.emplace_back([&, t] {
            std::mt19937 rng(99u + static_cast<unsigned>(t));
            std::uniform_int_distribution<int> coin(0, 3);
            const auto end = SteadyClock::now() + 500ms;
            while (SteadyClock::now() < end) {
                Record* rec;
                {
                    std::lock_guard<std::mutex> lock(recordsMutex);
                    rec = &records.emplace_back();
                }
                auto h = group.createContract([rec, &early, &twice]() noexcept {
                    if (SteadyClock::now().time_since_epoch().count() < rec->dueTicks.load()) early.fetch_add(1);
                    if (rec->runs.fetch_add(1) != 0) twice.fetch_add(1);
                });
                if (!h.valid()) {
                    std::this_thread::yield();
                    continue;
                }
                const bool timed = coin(rng) < 2;
                const auto due = timed ? SteadyClock::now() + 200us : SteadyClock::now();
                rec->dueTicks.store(due.time_since_epoch().count());
                EXPECT_EQ(timed ? h.scheduleAt(due) : h.schedule(), ScheduleResult::Scheduled);
                if (coin(rng) == 0) {
                    rec->released.store(true);
                    h.release();  // may race a peek, a claim, or an execution
                    released.fetch_add(1);
                }
            }
        });
    }
    for (auto& o : owners) o.join();

    const auto drainLimit = SteadyClock::now() + 5s;
    while (group.scheduledCount() > 0 && SteadyClock::now() < drainLimit) {
        std::this_thread::sleep_for(1ms);
    }
    stop.store(true, std::memory_order_release);
    for (auto& p : pullers) p.join();

    size_t lost = 0;
    for (const auto& rec : records) {
        if (!rec.released.load() && rec.dueTicks.load() != 0 && rec.runs.load() != 1) ++lost;
    }
    EXPECT_GT(released.load(), 0u);
    EXPECT_EQ(early.load(), 0u);
    EXPECT_EQ(twice.load(), 0u);
    EXPECT_EQ(lost, 0u);
    EXPECT_EQ(group.scheduledCount(), 0u);
    EXPECT_EQ(group.executingCount(), 0u);
    EXPECT_EQ(group.activeCount(), 0u);
    EXPECT_EQ(group.timedCount(), 0u);
}
