#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkGraph.h"

using namespace EntropyEngine::Core::Concurrency;

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
