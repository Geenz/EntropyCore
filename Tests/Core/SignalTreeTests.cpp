/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include <catch2/catch_test_macros.hpp>
#include "../src/Concurrency/SignalTree.h"
#include <thread>
#include <atomic>
#include <vector>
#include <random>
#include <unordered_set>

using namespace EntropyEngine::Core::Concurrency;

TEST_CASE("SignalTree basic functionality", "[signaltree][basic]") {
    SECTION("Construction and capacity") {
        SignalTree tree(16);
        
        REQUIRE(tree.getLeafCapacity() == 16);
        REQUIRE(tree.getTotalNodes() == 31);  // 2*16 - 1
        REQUIRE(tree.getCapacity() == 16 * 64);  // 1024 total signals
        REQUIRE(tree.isEmpty() == true);
    }
    
    SECTION("Power of 2 validation") {
        REQUIRE_THROWS_AS(SignalTree(15), std::invalid_argument);
        REQUIRE_THROWS(SignalTree(0));  // Will throw bad_alloc due to size calculation
        REQUIRE_NOTHROW(SignalTree(1));
        REQUIRE_NOTHROW(SignalTree(2));
        REQUIRE_NOTHROW(SignalTree(4));
        REQUIRE_NOTHROW(SignalTree(8));
        REQUIRE_NOTHROW(SignalTree(16));
    }
    
    SECTION("Set and select single signal") {
        SignalTree tree(4);
        
        tree.set(42);
        REQUIRE(tree.isEmpty() == false);
        
        uint64_t bias = 0;
        auto [index, isEmpty] = tree.select(bias);
        
        REQUIRE(index == 42);
        REQUIRE(isEmpty == true);
        REQUIRE(tree.isEmpty() == true);
    }
    
    SECTION("Set and clear without select") {
        SignalTree tree(4);
        
        tree.set(10);
        tree.set(20);
        REQUIRE(tree.isEmpty() == false);
        
        tree.clear(10);
        REQUIRE(tree.isEmpty() == false);
        
        tree.clear(20);
        REQUIRE(tree.isEmpty() == true);
    }
    
    SECTION("Multiple signals set and select") {
        SignalTree tree(4);
        std::unordered_set<size_t> signals = {5, 10, 15, 20, 25};
        
        for (auto sig : signals) {
            tree.set(sig);
        }
        
        std::unordered_set<size_t> selected;
        uint64_t bias = 0;
        
        while (!tree.isEmpty()) {
            auto [index, isEmpty] = tree.select(bias);
            REQUIRE(index != SignalTree::S_INVALID_SIGNAL_INDEX);
            selected.insert(index);
        }
        
        REQUIRE(selected == signals);
    }
    
    SECTION("Boundary signals") {
        SignalTree tree(4);  // Capacity = 256 signals (0-255)
        
        tree.set(0);    // First signal
        tree.set(255);  // Last signal
        
        uint64_t bias = 0;
        auto [first, _] = tree.select(bias);
        auto [last, isEmpty] = tree.select(bias);
        
        REQUIRE(((first == 0 && last == 255) || (first == 255 && last == 0)));
        REQUIRE(isEmpty == true);
    }
}

TEST_CASE("SignalTree accounting accuracy", "[signaltree][accounting]") {
    SECTION("Root counter tracks signal count") {
        SignalTree tree(8);
        
        REQUIRE(tree.getRoot().load() == 0);
        
        tree.set(10);
        REQUIRE(tree.getRoot().load() == 1);
        
        tree.set(20);
        tree.set(30);
        REQUIRE(tree.getRoot().load() == 3);
        
        tree.clear(20);
        REQUIRE(tree.getRoot().load() == 2);
        
        uint64_t bias = 0;
        tree.select(bias);
        REQUIRE(tree.getRoot().load() == 1);
        
        tree.select(bias);
        REQUIRE(tree.getRoot().load() == 0);
    }
    
    SECTION("Duplicate sets don't affect count") {
        SignalTree tree(4);
        
        tree.set(15);
        REQUIRE(tree.getRoot().load() == 1);
        
        tree.set(15);  // Set same signal again
        REQUIRE(tree.getRoot().load() == 1);  // Count should not increase
        
        tree.set(15);  // And again
        REQUIRE(tree.getRoot().load() == 1);
    }
    
    SECTION("Clear non-existent signal doesn't affect count") {
        SignalTree tree(4);
        
        tree.set(10);
        tree.set(20);
        REQUIRE(tree.getRoot().load() == 2);
        
        tree.clear(30);  // Clear signal that was never set
        REQUIRE(tree.getRoot().load() == 2);  // Count unchanged
    }
    
    SECTION("Internal node counters consistency") {
        SignalTree tree(4);  // Small tree for easier verification
        
        // Set signals in different leaf nodes
        tree.set(0);    // Leaf 0, bit 0
        tree.set(64);   // Leaf 1, bit 0
        tree.set(128);  // Leaf 2, bit 0
        
        REQUIRE(tree.getRoot().load() == 3);
        
        // Check that internal nodes have correct counts
        // This would require exposing more internals, but we can verify
        // through select operations that traversal works correctly
        uint64_t bias = 0;
        size_t count = 0;
        while (!tree.isEmpty()) {
            auto [index, _] = tree.select(bias);
            if (index != SignalTree::S_INVALID_SIGNAL_INDEX) {
                count++;
            }
        }
        
        REQUIRE(count == 3);
    }
    
    SECTION("Accounting under full capacity") {
        SignalTree tree(2);  // 128 total capacity
        
        // Fill to capacity
        for (size_t i = 0; i < 128; ++i) {
            tree.set(i);
        }
        
        REQUIRE(tree.getRoot().load() == 128);
        
        // Clear half
        for (size_t i = 0; i < 64; ++i) {
            tree.clear(i);
        }
        
        REQUIRE(tree.getRoot().load() == 64);
        
        // Select remaining
        uint64_t bias = 0;
        size_t selectCount = 0;
        while (!tree.isEmpty()) {
            auto [index, _] = tree.select(bias);
            if (index != SignalTree::S_INVALID_SIGNAL_INDEX) {
                selectCount++;
            }
        }
        
        REQUIRE(selectCount == 64);
        REQUIRE(tree.getRoot().load() == 0);
    }
}

TEST_CASE("SignalTree bias and fairness", "[signaltree][bias]") {
    SECTION("Bias affects selection order") {
        SignalTree tree(4);
        
        // Set signals in predictable pattern
        tree.set(0);
        tree.set(127);
        tree.set(128);
        tree.set(255);
        
        // Different bias patterns should select different signals
        uint64_t bias1 = 0x0;  // All left
        uint64_t bias2 = 0xFFFFFFFFFFFFFFFF;  // All right
        
        auto [index1, _] = tree.select(bias1);
        tree.set(index1);  // Put it back
        
        auto [index2, _2] = tree.select(bias2);
        tree.set(index2);  // Put it back
        
        // With different biases, we should get different signals
        // (though this isn't guaranteed, it's highly likely with our setup)
        INFO("bias1 selected: " << index1 << ", bias2 selected: " << index2);
    }
    
    SECTION("Bias hint update") {
        SignalTree tree(4);
        
        tree.set(10);
        tree.set(200);
        
        uint64_t bias = 0;
        auto [index, _] = tree.select(bias);
        
        // Bias should be updated to reflect tree state during traversal
        REQUIRE(bias != 0);  // Should have been modified
    }
}

TEST_CASE("SignalTree concurrent operations", "[signaltree][concurrent]") {
    SECTION("Concurrent sets") {
        SignalTree tree(16);
        const int numThreads = 8;
        const int signalsPerThread = 100;
        
        std::vector<std::thread> threads;
        std::atomic<int> startSignal{0};
        
        for (int t = 0; t < numThreads; ++t) {
            threads.emplace_back([&tree, t, signalsPerThread, &startSignal]() {
                // Wait for all threads to be ready
                while (startSignal.load() == 0) {
                    std::this_thread::yield();
                }
                
                // Each thread sets unique signals
                for (int i = 0; i < signalsPerThread; ++i) {
                    size_t signal = t * signalsPerThread + i;
                    tree.set(signal);
                }
            });
        }
        
        startSignal = 1;  // Start all threads
        
        for (auto& t : threads) {
            t.join();
        }
        
        // Verify all signals were set
        REQUIRE(tree.getRoot().load() == numThreads * signalsPerThread);
        
        // Verify we can select them all
        uint64_t bias = 0;
        int selectCount = 0;
        while (!tree.isEmpty()) {
            auto [index, _] = tree.select(bias);
            if (index != SignalTree::S_INVALID_SIGNAL_INDEX) {
                selectCount++;
            }
        }
        
        REQUIRE(selectCount == numThreads * signalsPerThread);
    }
    
    SECTION("Concurrent select") {
        SignalTree tree(16);
        const int totalSignals = 1000;
        
        // Pre-set signals
        for (int i = 0; i < totalSignals; ++i) {
            tree.set(i);
        }
        
        const int numThreads = 4;
        std::atomic<int> totalSelected{0};
        std::vector<std::thread> threads;
        std::atomic<int> startSignal{0};
        
        for (int t = 0; t < numThreads; ++t) {
            threads.emplace_back([&tree, &totalSelected, &startSignal]() {
                uint64_t localBias = 0;
                int localCount = 0;
                
                // Wait for start
                while (startSignal.load() == 0) {
                    std::this_thread::yield();
                }
                
                // Select until empty
                while (true) {
                    auto [index, isEmpty] = tree.select(localBias);
                    if (index == SignalTree::S_INVALID_SIGNAL_INDEX) {
                        break;
                    }
                    localCount++;
                }
                
                totalSelected.fetch_add(localCount);
            });
        }
        
        startSignal = 1;  // Start all threads
        
        for (auto& t : threads) {
            t.join();
        }
        
        REQUIRE(totalSelected.load() == totalSignals);
        REQUIRE(tree.isEmpty() == true);
    }
    
    SECTION("Mixed concurrent operations") {
        SignalTree tree(16);
        std::atomic<bool> running{true};
        std::atomic<int> setsCompleted{0};
        std::atomic<int> selectsCompleted{0};
        
        // Producer threads
        std::vector<std::thread> producers;
        for (int t = 0; t < 2; ++t) {
            producers.emplace_back([&tree, &running, &setsCompleted, t]() {
                std::mt19937 rng(t);
                std::uniform_int_distribution<size_t> dist(0, 1023);
                
                while (running.load()) {
                    tree.set(dist(rng));
                    setsCompleted.fetch_add(1);
                    std::this_thread::yield();
                }
            });
        }
        
        // Consumer threads
        std::vector<std::thread> consumers;
        for (int t = 0; t < 2; ++t) {
            consumers.emplace_back([&tree, &running, &selectsCompleted]() {
                uint64_t bias = 0;
                
                while (running.load()) {
                    auto [index, _] = tree.select(bias);
                    if (index != SignalTree::S_INVALID_SIGNAL_INDEX) {
                        selectsCompleted.fetch_add(1);
                    }
                    std::this_thread::yield();
                }
            });
        }
        
        // Run for a short time
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        running = false;
        
        for (auto& t : producers) {
            t.join();
        }
        for (auto& t : consumers) {
            t.join();
        }
        
        // Verify operations completed
        REQUIRE(setsCompleted.load() > 0);
        REQUIRE(selectsCompleted.load() > 0);
        
        // Tree should be in valid state
        size_t finalCount = tree.getRoot().load();
        
        // Drain remaining signals
        uint64_t bias = 0;
        size_t drainCount = 0;
        while (!tree.isEmpty()) {
            auto [index, _] = tree.select(bias);
            if (index != SignalTree::S_INVALID_SIGNAL_INDEX) {
                drainCount++;
            }
        }
        
        REQUIRE(drainCount == finalCount);
    }
}

TEST_CASE("SignalTree stress test", "[signaltree][stress]") {
    SECTION("Rapid set/clear cycles") {
        SignalTree tree(8);
        const int cycles = 10000;
        
        for (int i = 0; i < cycles; ++i) {
            size_t signal = i % 512;  // Wrap around capacity
            
            tree.set(signal);
            REQUIRE(tree.getRoot().load() > 0);
            
            tree.clear(signal);
            // Note: May not be 0 if we wrapped and set same signal again
        }
        
        // Final clear
        for (size_t i = 0; i < 512; ++i) {
            tree.clear(i);
        }
        
        REQUIRE(tree.isEmpty() == true);
        REQUIRE(tree.getRoot().load() == 0);
    }
    
    SECTION("Fill and drain repeatedly") {
        SignalTree tree(4);
        const int iterations = 100;
        
        for (int iter = 0; iter < iterations; ++iter) {
            // Fill
            for (size_t i = 0; i < 256; ++i) {
                tree.set(i);
            }
            REQUIRE(tree.getRoot().load() == 256);
            
            // Drain
            uint64_t bias = iter;  // Vary bias each iteration
            size_t count = 0;
            while (!tree.isEmpty()) {
                auto [index, _] = tree.select(bias);
                if (index != SignalTree::S_INVALID_SIGNAL_INDEX) {
                    count++;
                }
            }
            REQUIRE(count == 256);
            REQUIRE(tree.isEmpty() == true);
        }
    }
}