#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkGraph.h"

using namespace EntropyEngine::Core::Concurrency;
using Clock = std::chrono::steady_clock;

namespace
{
// Drains the group until the graph completes (single-threaded executor)
void drain(WorkContractGroup& group, WorkGraph& graph) {
    while (!graph.isComplete()) {
        group.executeAllBackgroundWork();
        graph.processDeferredNodes();
        graph.checkTimedDeferrals();
    }
}
}  // namespace

TEST(WorkGraph, DiamondDependency_ExecutesInOrder) {
    WorkContractGroup group(64, "GraphDiamond");
    WorkGraph graph(&group);

    std::atomic<int> order{0};
    int a = -1, b = -1, c = -1, d = -1;

    auto na = graph.addNode([&]() { a = order.fetch_add(1); }, "A");
    auto nb = graph.addNode([&]() { b = order.fetch_add(1); }, "B");
    auto nc = graph.addNode([&]() { c = order.fetch_add(1); }, "C");
    auto nd = graph.addNode([&]() { d = order.fetch_add(1); }, "D");

    graph.addDependency(na, nb);
    graph.addDependency(na, nc);
    graph.addDependency(nb, nd);
    graph.addDependency(nc, nd);

    graph.execute();
    drain(group, graph);

    auto result = graph.wait();
    EXPECT_TRUE(result.allCompleted);
    EXPECT_EQ(result.completedCount, 4u);
    EXPECT_EQ(a, 0);          // root first
    EXPECT_EQ(d, 3);          // join last
    EXPECT_GT(b, a);
    EXPECT_GT(c, a);
    EXPECT_LT(b, d);
    EXPECT_LT(c, d);
}

TEST(WorkGraph, StructureFrozenDuringRun_MutatorsThrow) {
    WorkContractGroup group(64, "GraphFrozen");
    WorkGraph graph(&group);

    auto n1 = graph.addNode([]() {}, "N1");
    auto n2 = graph.addNode([]() {}, "N2");
    graph.addDependency(n1, n2);

    graph.execute();

    // Build once, execute many: structure mutation is illegal until reset()
    EXPECT_THROW(graph.addNode([]() {}, "Illegal"), std::logic_error);
    EXPECT_THROW(graph.addDependency(n1, n2), std::logic_error);

    drain(group, graph);
    auto result = graph.wait();
    EXPECT_TRUE(result.allCompleted);

    // Still frozen after completion; reset() re-arms building
    EXPECT_THROW(graph.addNode([]() {}, "StillIllegal"), std::logic_error);
    graph.reset();
    EXPECT_NO_THROW(graph.addNode([]() {}, "LegalAgain"));
}

TEST(WorkGraph, ResetAndReExecute_RunsTwice) {
    WorkContractGroup group(64, "GraphReRun");
    WorkGraph graph(&group);

    std::atomic<int> runs{0};
    auto n1 = graph.addNode([&]() { runs.fetch_add(1); }, "N1");
    auto n2 = graph.addNode([&]() { runs.fetch_add(1); }, "N2");
    graph.addDependency(n1, n2);

    graph.execute();
    drain(group, graph);
    EXPECT_TRUE(graph.wait().allCompleted);
    EXPECT_EQ(runs.load(), 2);

    graph.reset();
    graph.execute();
    drain(group, graph);
    EXPECT_TRUE(graph.wait().allCompleted);
    EXPECT_EQ(runs.load(), 4);
}

TEST(WorkGraph, FailingNode_CancelsDependents) {
    WorkContractGroup group(64, "GraphFail");
    WorkGraph graph(&group);

    std::atomic<int> ran{0};
    auto na = graph.addNode([]() { throw std::runtime_error("boom"); }, "Failing");
    auto nb = graph.addNode([&]() { ran.fetch_add(1); }, "Dependent");
    auto nc = graph.addNode([&]() { ran.fetch_add(1); }, "Independent");
    graph.addDependency(na, nb);
    (void)nc;

    graph.execute();
    drain(group, graph);

    auto result = graph.wait();
    EXPECT_FALSE(result.allCompleted);
    EXPECT_EQ(result.failedCount, 1u);
    EXPECT_EQ(result.completedCount, 1u);  // only the independent node
    EXPECT_EQ(ran.load(), 1);
}

TEST(WorkGraph, DeferredNodes_CompleteWhenCapacityFrees) {
    // Group capacity far smaller than node count forces the deferred path
    // (regression: processDeferredNodes abandoned extracted nodes on a failed
    // schedule, stranding wait() forever).
    WorkContractGroup group(8, "GraphDeferred");
    WorkGraph graph(&group);

    std::atomic<int> ran{0};
    for (int i = 0; i < 128; ++i) {
        graph.addNode([&ran]() { ran.fetch_add(1); }, "N");
    }

    graph.execute();
    drain(group, graph);

    auto result = graph.wait();
    EXPECT_TRUE(result.allCompleted);
    EXPECT_EQ(result.completedCount, 128u);
    EXPECT_EQ(ran.load(), 128);
}

TEST(WorkGraph, YieldUntilDoesNotRunBeforeWakeTime) {
    WorkContractGroup group(64, "GraphYieldUntilWake");
    WorkGraph graph(&group);

    std::atomic<int> runs{0};
    Clock::time_point wakeTime{};
    Clock::time_point secondRunAt{};

    graph.addYieldableNode(
        [&]() -> WorkResultContext {
            if (runs.fetch_add(1) == 0) {
                wakeTime = Clock::now() + std::chrono::seconds(1);  // wide margin for the "not yet" checks
                return WorkResultContext::yieldUntil(wakeTime);
            }
            secondRunAt = Clock::now();
            return WorkResultContext::complete();
        },
        "yield-until-once");

    graph.execute();
    group.executeAllBackgroundWork();
    ASSERT_EQ(runs.load(), 1);

    // Pumping before the wake time does not run the node
    EXPECT_EQ(graph.checkTimedDeferrals(), 0u);
    group.executeAllBackgroundWork();
    EXPECT_EQ(runs.load(), 1);

    drain(group, graph);

    EXPECT_EQ(runs.load(), 2);
    EXPECT_GE(secondRunAt, wakeTime);
    EXPECT_TRUE(graph.wait().allCompleted);
}

TEST(WorkGraph, YieldUntilHoldsOneContractSlot) {
    WorkContractGroup group(64, "GraphYieldUntilSlot");
    WorkGraph graph(&group);

    std::atomic<int> runs{0};
    graph.addYieldableNode(
        [&]() -> WorkResultContext {
            if (runs.fetch_add(1) == 0) {
                return WorkResultContext::yieldUntil(Clock::now() + std::chrono::milliseconds(100));
            }
            return WorkResultContext::complete();
        },
        "yield-until-slot");

    graph.execute();
    group.executeAllBackgroundWork();
    ASSERT_EQ(runs.load(), 1);

    // The waiting node is one scheduled timed contract, skipped until due
    EXPECT_EQ(group.activeCount(), 1u);
    EXPECT_EQ(group.scheduledCount(), 1u);
    EXPECT_EQ(group.timedCount(), 1u);

    drain(group, graph);

    EXPECT_EQ(runs.load(), 2);
    EXPECT_EQ(group.activeCount(), 0u);
}

TEST(WorkGraph, YieldUntilFallsBackWhenGroupFull) {
    WorkContractGroup group(2, "GraphYieldUntilFull");
    WorkGraph graph(&group);

    std::vector<WorkContractHandle> blockers;
    std::atomic<int> runs{0};
    Clock::time_point wakeTime{};
    Clock::time_point secondRunAt{};

    graph.addYieldableNode(
        [&]() -> WorkResultContext {
            if (runs.fetch_add(1) == 0) {
                // Take every free slot so the timed contract cannot be created
                for (;;) {
                    auto blocker = group.createContract([]() {});
                    if (!blocker.valid()) break;
                    blockers.push_back(blocker);
                }
                wakeTime = Clock::now() + std::chrono::milliseconds(30);
                return WorkResultContext::yieldUntil(wakeTime);
            }
            secondRunAt = Clock::now();
            return WorkResultContext::complete();
        },
        "yield-until-full");

    graph.execute();
    group.executeAllBackgroundWork();
    ASSERT_EQ(runs.load(), 1);
    ASSERT_FALSE(blockers.empty());

    // The node waits for capacity, not for time: still parked after its wake time
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    graph.checkTimedDeferrals();
    group.executeAllBackgroundWork();
    EXPECT_EQ(runs.load(), 1);

    // Freeing a slot re-arms the node with its original (now past) wake time
    for (auto& blocker : blockers) {
        blocker.release();
    }
    blockers.clear();

    drain(group, graph);

    EXPECT_EQ(runs.load(), 2);
    EXPECT_GE(secondRunAt, wakeTime);
    EXPECT_TRUE(graph.wait().allCompleted);
}

TEST(WorkGraph, YieldUntilNodeReachesCompleted) {
    WorkContractGroup group(64, "GraphYieldUntilCompleted");
    WorkGraph graph(&group);
    std::atomic<int> runs{0};
    graph.addYieldableNode(
        [&]() -> WorkResultContext {
            if (runs.fetch_add(1) == 0) return WorkResultContext::yieldUntil(Clock::now());
            return WorkResultContext::complete();
        },
        "yield-until-completed");

    graph.execute();
    drain(group, graph);

    EXPECT_EQ(runs.load(), 2);
    EXPECT_EQ(graph.getStats().completedNodes, 1u) << "the node must reach Completed, not stay Yielded";
}

TEST(WorkGraph, YieldUntilNodeReachesFailed) {
    WorkContractGroup group(64, "GraphYieldUntilFailed");
    WorkGraph graph(&group);
    std::atomic<int> runs{0};
    graph.addYieldableNode(
        [&]() -> WorkResultContext {
            if (runs.fetch_add(1) == 0) return WorkResultContext::yieldUntil(Clock::now());
            throw std::runtime_error("second run fails");
        },
        "yield-until-failed");

    graph.execute();
    drain(group, graph);

    EXPECT_EQ(runs.load(), 2);
    EXPECT_EQ(graph.getStats().failedNodes, 1u) << "the node must reach Failed, not stay Yielded";
}

TEST(WorkGraph, YieldUntilWhileSuspendedWaitsForResume) {
    WorkContractGroup group(64, "GraphYieldUntilSuspended");
    WorkGraph graph(&group);
    std::atomic<int> runs{0};
    graph.addYieldableNode(
        [&]() -> WorkResultContext {
            if (runs.fetch_add(1) == 0) {
                graph.suspend();
                return WorkResultContext::yieldUntil(Clock::now());  // due at once, but suspended
            }
            return WorkResultContext::complete();
        },
        "yield-until-suspended");

    graph.execute();
    group.executeAllBackgroundWork();
    graph.checkTimedDeferrals();
    group.executeAllBackgroundWork();
    EXPECT_EQ(runs.load(), 1) << "a node that yields while the graph is suspended must not run until resume()";
    EXPECT_EQ(group.scheduledCount(), 0u);

    graph.resume();
    drain(group, graph);
    EXPECT_EQ(runs.load(), 2);
    EXPECT_EQ(graph.getStats().completedNodes, 1u);
}

TEST(WorkGraph, YieldUntilResumedBeforeWakeTimeWaitsForIt) {
    WorkContractGroup group(64, "GraphYieldUntilResumeEarly");
    WorkGraph graph(&group);
    std::atomic<int> runs{0};
    Clock::time_point wakeTime{};
    Clock::time_point secondRunAt{};
    graph.addYieldableNode(
        [&]() -> WorkResultContext {
            if (runs.fetch_add(1) == 0) {
                graph.suspend();
                wakeTime = Clock::now() + std::chrono::seconds(1);  // wide margin for the "not yet" check
                return WorkResultContext::yieldUntil(wakeTime);
            }
            secondRunAt = Clock::now();
            return WorkResultContext::complete();
        },
        "yield-until-resume-early");

    graph.execute();
    group.executeAllBackgroundWork();
    ASSERT_EQ(runs.load(), 1);

    graph.resume();  // well before the wake time
    group.executeAllBackgroundWork();
    EXPECT_EQ(runs.load(), 1) << "resume() must not run a yield-until node before its wake time";
    EXPECT_EQ(group.timedCount(), 1u);

    drain(group, graph);
    EXPECT_EQ(runs.load(), 2);
    EXPECT_GE(secondRunAt, wakeTime);
}

TEST(WorkGraph, GraphDestroyedWhileYieldWaiting) {
    WorkContractGroup group(64, "GraphYieldUntilDestroy");

    std::atomic<int> runs{0};
    auto graph = std::make_unique<WorkGraph>(&group);
    graph->addYieldableNode(
        [&]() -> WorkResultContext {
            runs.fetch_add(1);
            return WorkResultContext::yieldUntil(Clock::now() + std::chrono::seconds(30));
        },
        "yield-until-destroyed");

    graph->execute();
    group.executeAllBackgroundWork();
    ASSERT_EQ(runs.load(), 1);
    ASSERT_EQ(group.activeCount(), 1u);

    // The destructor drains without waiting for the timed contract
    const auto destroyStart = Clock::now();
    graph.reset();
    EXPECT_LT(Clock::now() - destroyStart, std::chrono::seconds(10));  // did not wait for the 30 s wake time

    EXPECT_EQ(group.activeCount(), 0u);
    group.executeAllBackgroundWork();
    EXPECT_EQ(runs.load(), 1);
}
