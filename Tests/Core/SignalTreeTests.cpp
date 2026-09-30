#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "Concurrency/SignalTree.h"

using EntropyEngine::Core::Concurrency::SignalTree;

TEST(SignalTree, SmokeBuildsAndRuns) {
    SUCCEED();
}

TEST(SignalTree, PeekFindsFirstSetSignalAtOrAfter) {
    SignalTree tree(4);  // 256 signals
    constexpr size_t NONE = SignalTree::S_INVALID_SIGNAL_INDEX;
    EXPECT_EQ(tree.peek(0), NONE);

    tree.set(3);
    tree.set(64);
    tree.set(200);
    EXPECT_EQ(tree.peek(0), 3u);
    EXPECT_EQ(tree.peek(3), 3u);
    EXPECT_EQ(tree.peek(4), 64u);   // crosses a leaf boundary
    EXPECT_EQ(tree.peek(64), 64u);
    EXPECT_EQ(tree.peek(65), 200u);  // skips an empty leaf
    EXPECT_EQ(tree.peek(201), NONE);
    EXPECT_EQ(tree.peek(256), NONE);  // past capacity

    tree.set(255);
    EXPECT_EQ(tree.peek(201), 255u);
    EXPECT_EQ(tree.peek(255), 255u);
}

TEST(SignalTree, PeekLeavesTreeUnchanged) {
    SignalTree tree(8);  // 512 signals
    for (size_t i = 0; i < 512; i += 7) tree.set(i);

    std::vector<uint64_t> before;
    for (size_t n = 0; n < tree.getTotalNodes(); ++n) before.push_back(tree.getNode(n).load());

    size_t seen = 0;
    for (size_t i = tree.peek(0); i != SignalTree::S_INVALID_SIGNAL_INDEX; i = tree.peek(i + 1)) {
        EXPECT_EQ(i % 7, 0u);
        ++seen;
    }
    EXPECT_EQ(seen, (511 / 7) + 1);

    for (size_t n = 0; n < tree.getTotalNodes(); ++n) {
        EXPECT_EQ(tree.getNode(n).load(), before[n]) << "node " << n;
    }
}

TEST(SignalTree, ConcurrentPeekReturnsOnlySignalsThatWereSet) {
    SignalTree tree(4);  // 256 signals; only even indices are ever set
    std::atomic<bool> stop{false};
    std::atomic<size_t> bad{0};
    std::atomic<size_t> hits{0};

    std::thread toggler([&] {
        size_t i = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const size_t index = (i * 2) % 256;
            if ((i / 128) % 2 == 0) {
                tree.set(index);
            } else {
                tree.clear(index);
            }
            ++i;
        }
    });

    std::vector<std::thread> peekers;
    for (int t = 0; t < 3; ++t) {
        peekers.emplace_back([&, t] {
            size_t from = static_cast<size_t>(t) * 17;
            for (int n = 0; n < 200000; ++n) {
                const size_t found = tree.peek(from % 256);
                if (found != SignalTree::S_INVALID_SIGNAL_INDEX) {
                    if (found % 2 != 0 || found < from % 256) bad.fetch_add(1);
                    hits.fetch_add(1, std::memory_order_relaxed);
                }
                from += 13;
            }
        });
    }
    for (auto& p : peekers) p.join();
    stop.store(true, std::memory_order_release);
    toggler.join();

    EXPECT_EQ(bad.load(), 0u);
    EXPECT_GT(hits.load(), 0u);
}
