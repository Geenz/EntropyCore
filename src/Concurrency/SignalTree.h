/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file SignalTree.h
 * @brief Lock-free binary tree for concurrent signal management
 *
 * This file contains the SignalTree implementation, a specialized data structure
 * that provides lock-free signal setting, selection, and clearing operations.
 * It's designed for scenarios where multiple threads need to coordinate
 * work distribution without contention.
 */

#pragma once
#include <atomic>
#include <bit>      // For std::countr_zero
#include <cstdint>  // For uint64_t
#include <memory>
#include <stdexcept>  // For std::invalid_argument

#include "../CoreCommon.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

/**
 * @brief Abstract base class for SignalTree to enable polymorphic usage
 *
 * This allows WorkContractGroup to use SignalTree instances of different
 * sizes without templates, selected at runtime based on capacity needs.
 */
class SignalTreeBase
{
public:
    static constexpr size_t S_INVALID_SIGNAL_INDEX = ~0ULL;

    virtual ~SignalTreeBase() = default;

    virtual void set(size_t leafIndex) = 0;
    virtual std::pair<size_t, bool> select(uint64_t& biasFlags) = 0;
    virtual void clear(size_t leafIndex) = 0;
    virtual bool isEmpty() const = 0;
    virtual size_t getCapacity() const = 0;
};

/**
 * @brief A lock-free binary tree for signal selection and management.
 *
 * A signal dispatcher that can handle large numbers of signals without
 * locks. Suitable for work-stealing schedulers, event systems, or any scenario
 * requiring atomic signal selection and processing from multiple threads.
 *
 * The key innovation is the tree structure - internal nodes track active signal counts
 * in their subtrees, while leaf nodes pack 64 signals each into bit fields.
 * This provides O(log n) signal selection with excellent cache coherence.
 *
 * Key features:
 * - **Lock-free**: Multiple threads can set/select signals concurrently
 * - **Cache-friendly**: Entire tree lives in a contiguous array
 * - **Scalable**: Supports LeafCapacity * 64 total signals
 * - **Fair**: The bias system prevents signal starvation
 *
 * @tparam LeafCapacity Number of leaf nodes (must be power of 2). Total signal capacity is LeafCapacity * 64.
 *
 * @code
 * // Complete multi-threaded workflow
 * SignalTree tree(4);  // 256 signals capacity
 * std::atomic<bool> running{true};
 *
 * // Producer threads: submit work signals
 * std::thread producer([&tree]() {
 *     for (int i = 0; i < 100; ++i) {
 *         tree.set(i % 256);  // Set signal
 *         std::this_thread::sleep_for(1ms);
 *     }
 * });
 *
 * // Consumer threads: process signals with fairness
 * std::thread consumer([&tree, &running]() {
 *     uint64_t bias = 0;
 *     while (running) {
 *         auto [index, found] = tree.select(bias);
 *         if (found) {
 *             processSignal(index);
 *             // Rotate bias for fairness
 *             bias = (bias << 1) | (bias >> 63);
 *         } else {
 *             std::this_thread::yield();
 *         }
 *     }
 * });
 *
 * producer.join();
 * running = false;
 * consumer.join();
 * @endcode
 */
class SignalTree : public SignalTreeBase
{
private:
    const size_t _leafCapacity;
    const size_t _totalNodes;
    std::unique_ptr<std::atomic<uint64_t>[]>
        _nodes;  ///< Tree storage: internal nodes are counters, leaf nodes are bitmaps

    /**
     * @brief Runtime power-of-2 validation helper
     *
     * This is static because it's a pure utility function with no dependencies
     * on instance state. Making it static clarifies that it has no side effects
     * and can be used during construction before the object is fully initialized.
     *
     * @param n Number to check
     * @return true if n is a power of 2 and greater than 0
     */
    static bool isPowerOf2(size_t n) {
        return n > 0 && (n & (n - 1)) == 0;
    }

    static constexpr uint64_t S_BIT_ONE = 1ULL;  ///< Single bit constant
    static constexpr size_t S_BITS_PER_LEAF_NODE =
        64;  ///< Bits per uint64_t leaf - constexpr because this is a fundamental architectural constant

    static constexpr uint64_t S_BIAS_BIT_START = 1ULL;  ///< Starting bit for bias traversal
    static constexpr size_t S_BIAS_SHIFT_AMOUNT = 1;    ///< Bit shift for bias progression

public:
    static constexpr size_t S_INVALID_SIGNAL_INDEX = ~0ULL;  ///< Returned when no signal is available

    enum class TreePath
    {
        Left = 1,
        Right = 2
    };
    /**
     * @brief Constructs a SignalTree with specified leaf capacity
     * @param leafCapacity Number of leaf nodes (must be power of 2)
     * @throws std::invalid_argument if leafCapacity is not a power of 2
     */
    explicit SignalTree(size_t leafCapacity)
        : _leafCapacity(leafCapacity),
          _totalNodes(2 * leafCapacity - 1),
          _nodes(std::make_unique<std::atomic<uint64_t>[]>(_totalNodes)) {
        if (!isPowerOf2(_leafCapacity)) {
            throw std::invalid_argument("LeafCapacity must be a power of 2 and greater than 0");
        }

        // Initialize all atomics to 0
        for (size_t i = 0; i < _totalNodes; ++i) {
            _nodes[i].store(0, std::memory_order_relaxed);
        }
    }

    SignalTree(const SignalTree&) = delete;
    SignalTree& operator=(const SignalTree&) = delete;
    SignalTree(SignalTree&&) = delete;             // Deleted move constructor
    SignalTree& operator=(SignalTree&&) = delete;  // Deleted move assignment operator

    /**
     * @brief Gets direct access to the root node
     *
     * Advanced use only. Root value = total active signals.
     *
     * @return Reference to the atomic root node counter
     */
    std::atomic<uint64_t>& getRoot() {
        return _nodes[0];
    }

    /**
     * @brief Gets a child node given parent index and direction
     *
     * Internal navigation helper for tree traversal.
     *
     * @param parent Index of the parent node
     * @param path Which child to get (Left or Right)
     * @return Reference to the child node
     */
    std::atomic<uint64_t>& getChild(size_t parent, TreePath path) {
        return _nodes[parent * 2 + static_cast<size_t>(path)];
    }

    /**
     * @brief Calculates child node index without accessing the node
     *
     * @param parent Index of the parent node
     * @param path Which child (Left or Right)
     * @return Index of the child node in the internal array
     */
    size_t getChildIndex(size_t parent, TreePath path) const {
        size_t childIndex = parent * 2 + static_cast<size_t>(path);
        ENTROPY_ASSERT(childIndex < _totalNodes, "Child index out of bounds!");
        return childIndex;
    }

    /**
     * @brief Direct access to any node by index
     *
     * Low-level access. Internal nodes: 0 to LeafCapacity-2, leaf nodes: rest.
     *
     * @param index Node index in the internal array
     * @return Reference to the atomic node
     */
    std::atomic<uint64_t>& getNode(size_t index) {
        ENTROPY_ASSERT(index < _totalNodes, "Node index out of bounds!");
        return _nodes[index];
    }

    /**
     * @brief Calculates parent node index for tree traversal
     *
     * Formula: parent = (child - 1) / 2
     *
     * @param child Index of the child node (must not be root)
     * @return Index of the parent node
     */
    size_t getParentIndex(size_t child) const {
        ENTROPY_ASSERT(child > 0 && child < _totalNodes, "Cannot get parent of root or invalid index!");
        return (child - 1) / 2;
    }

    /**
     * @brief Gets the number of leaf nodes in the tree
     * @return LeafCapacity template parameter value
     */
    size_t getLeafCapacity() const {
        return _leafCapacity;
    }

    /**
     * @brief Gets total number of nodes in the tree (internal + leaf)
     * @return Total node count (always 2*LeafCapacity - 1)
     */
    size_t getTotalNodes() const {
        return _totalNodes;
    }

    /**
     * @brief Sets a signal as active in the tree
     *
     * Thread-safe and lock-free. Updates internal counters up to root.
     *
     * @param leafIndex Signal index to set (0 to LeafCapacity*64-1)
     *
     * @code
     * // Worker thread marks task 42 as ready
     * signals.set(42);
     *
     * // Multiple threads can set signals concurrently
     * std::thread t1([&]() { signals.set(10); });
     * std::thread t2([&]() { signals.set(20); });
     * @endcode
     */
    void set(size_t leafIndex) override {
        // 1. Input Validation
        ENTROPY_ASSERT(leafIndex < _leafCapacity * S_BITS_PER_LEAF_NODE, "Leaf index out of bounds!");

        // 2. Calculate Leaf Node Array Index
        // Leaf nodes start after all internal nodes.
        // The number of internal nodes is LeafCapacity - 1 (for a complete binary tree)
        // So, the first leaf node is at index (LeafCapacity - 1).\
        // Each leaf node (uint64_t) can hold 64 signals.
        size_t leafNodeArrayStartIndex = _totalNodes - _leafCapacity;
        size_t leafNodeOffsetInArray = leafIndex / S_BITS_PER_LEAF_NODE;  // Which uint64_t leaf node
        size_t actualLeafNodeIndex = leafNodeArrayStartIndex + leafNodeOffsetInArray;

        // 3. Calculate Bit Position within Leaf Node
        size_t bitPos = leafIndex % S_BITS_PER_LEAF_NODE;

        // 4. Atomically Set Bit and get the OLD value
        uint64_t oldValue = _nodes[actualLeafNodeIndex].fetch_or(S_BIT_ONE << bitPos, std::memory_order_release);

        // 5. Propagate Up only if the bit was not already set
        if (!(oldValue & (S_BIT_ONE << bitPos))) {
            // The bit was 0 before, so we need to update the counters.
            size_t currentNodeIndex = actualLeafNodeIndex;
            while (currentNodeIndex > 0) {  // Stop when we reach the root (index 0)
                size_t parentIndex = getParentIndex(currentNodeIndex);
                // Atomically increment the parent's counter
                // Use memory_order_relaxed as we are only concerned with the total count,
                // and the ordering is handled by the leaf node's fetch_or.
                _nodes[parentIndex].fetch_add(1, std::memory_order_relaxed);
                currentNodeIndex = parentIndex;
            }
        }
    }

    /**
     * @brief Selects and clears an active signal from the tree
     *
     * Atomically finds, clears and returns signal index. Lock-free. Bias controls
     * fairness by guiding traversal (each bit chooses left/right at each level).
     *
     * @param biasFlags Bit pattern controlling traversal (LSB at root, shifts left)
     * @return Pair of {signal_index, tree_is_empty}. signal_index is S_INVALID_SIGNAL_INDEX if none available
     *
     * @code
     * // Basic work-stealing loop with empty detection
     * uint64_t bias = 0;
     * while (running) {
     *     auto [signal, isEmpty] = signals.select(bias);
     *     if (signal != SignalTree<4>::S_INVALID_SIGNAL_INDEX) {
     *         processWork(signal);
     *         if (isEmpty) {
     *             // Tree is now empty, might want to steal work
     *         }
     *     } else {
     *         // No work available, steal from another queue
     *     }
     *     bias = rotateBias(bias);  // Ensure fairness
     * }
     * @endcode
     */
    std::pair<size_t, bool> select(uint64_t& biasFlags) override {
        size_t currentNodeIndex = 0;                 // Start at the root
        uint64_t localBiasHint = 0;                  // Build up bias hint during traversal
        uint64_t currentBiasBit = S_BIAS_BIT_START;  // Start with LSB

        // Traverse down the tree to find the leaf node
        while (currentNodeIndex < _totalNodes - _leafCapacity) {  // While not a leaf node
            uint64_t leftChildValue =
                _nodes[getChildIndex(currentNodeIndex, TreePath::Left)].load(std::memory_order_acquire);
            uint64_t rightChildValue =
                _nodes[getChildIndex(currentNodeIndex, TreePath::Right)].load(std::memory_order_acquire);

            // Use current bias bit to decide which child to prioritize (LSB approach)
            bool biasRight = (biasFlags & currentBiasBit) != 0;
            bool chooseRight = (biasRight && rightChildValue > 0) || (leftChildValue == 0);

            // Build bias hint: set current bit if right child has work
            if (rightChildValue > 0) {
                localBiasHint |= currentBiasBit;
            }

            if (chooseRight && rightChildValue > 0) {
                currentNodeIndex = getChildIndex(currentNodeIndex, TreePath::Right);
            } else if (leftChildValue > 0) {
                currentNodeIndex = getChildIndex(currentNodeIndex, TreePath::Left);
            } else {
                return {S_INVALID_SIGNAL_INDEX, true};  // No active signals, tree is empty
            }

            currentBiasBit <<= S_BIAS_SHIFT_AMOUNT;  // Move to next higher bit (LSB to MSB)
        }

        // Now current_node_index is a leaf node (or the start of a block of leaf nodes)
        // We need to find an active bit within this leaf node's uint64_t

        uint64_t leafValueExpected;
        size_t bitPos;
        bool success = false;

        // Retry loop for compare_exchange_weak
        do {
            leafValueExpected = _nodes[currentNodeIndex].load(std::memory_order_acquire);
            if (leafValueExpected == 0) {
                return {S_INVALID_SIGNAL_INDEX, true};  // Leaf node became empty, no signal found
            }

            // Find the first set bit (LSB) using C++20 standard function
            bitPos = std::countr_zero(leafValueExpected);

            // Attempt to atomically clear the bit
            // If this fails, leafValueExpected is updated with the current value, and the loop retries.
            success = _nodes[currentNodeIndex].compare_exchange_weak(
                leafValueExpected, leafValueExpected & ~(S_BIT_ONE << bitPos), std::memory_order_release,
                std::memory_order_relaxed);
        } while (!success);  // Keep retrying until compare_exchange_weak succeeds

        // Update caller's bias with the pattern we found
        biasFlags = localBiasHint;

        // Calculate global leaf index
        size_t leafNodeArrayStartIndex = _totalNodes - _leafCapacity;
        size_t leafNodeOffsetInArray = currentNodeIndex - leafNodeArrayStartIndex;
        size_t globalLeafIndex = (leafNodeOffsetInArray * S_BITS_PER_LEAF_NODE) + bitPos;

        // Propagate Up (Decrement Counters)
        size_t tempNodeIndex = currentNodeIndex;
        while (tempNodeIndex > 0) {
            size_t parentIndex = getParentIndex(tempNodeIndex);
            _nodes[parentIndex].fetch_sub(1, std::memory_order_relaxed);
            tempNodeIndex = parentIndex;
        }
        bool treeIsEmpty = (_nodes[0].load(std::memory_order_acquire) == 0);
        return {globalLeafIndex, treeIsEmpty};
    }

    /**
     * @brief Alias for set() - signals a contract is ready for execution
     * @param leafIndex Signal index to signal (0 to LeafCapacity*64-1)
     */
    void signal(size_t leafIndex) {
        set(leafIndex);
    }

    /**
     * @brief Clears a signal from the tree without selecting it
     * @param leafIndex Signal index to clear (0 to LeafCapacity*64-1)
     */
    void clear(size_t leafIndex) override {
        // Input validation
        ENTROPY_ASSERT(leafIndex < _leafCapacity * S_BITS_PER_LEAF_NODE, "Leaf index out of bounds!");

        // Calculate leaf node position
        size_t leafNodeArrayStartIndex = _totalNodes - _leafCapacity;
        size_t leafNodeOffsetInArray = leafIndex / S_BITS_PER_LEAF_NODE;
        size_t actualLeafNodeIndex = leafNodeArrayStartIndex + leafNodeOffsetInArray;

        // Calculate bit position
        size_t bitPos = leafIndex % S_BITS_PER_LEAF_NODE;

        // Atomically clear the bit
        uint64_t oldValue = _nodes[actualLeafNodeIndex].fetch_and(~(S_BIT_ONE << bitPos), std::memory_order_release);

        // Only propagate if the bit was actually set
        if (oldValue & (S_BIT_ONE << bitPos)) {
            // Propagate up (decrement counters)
            size_t currentNodeIndex = actualLeafNodeIndex;
            while (currentNodeIndex > 0) {
                size_t parentIndex = getParentIndex(currentNodeIndex);
                _nodes[parentIndex].fetch_sub(1, std::memory_order_relaxed);
                currentNodeIndex = parentIndex;
            }
        }
    }

    /**
     * @brief Checks if the tree has no active signals
     * @return true if no signals are set
     */
    bool isEmpty() const override {
        return _nodes[0].load(std::memory_order_acquire) == 0;
    }

    /**
     * @brief Gets the total capacity of this SignalTree
     * @return Maximum number of signals (LeafCapacity * 64)
     */
    size_t getCapacity() const override {
        return _leafCapacity * S_BITS_PER_LEAF_NODE;
    }

    /**
     * @brief Constant for invalid signal (alias for compatibility)
     */
    static constexpr size_t INVALID_SIGNAL = S_INVALID_SIGNAL_INDEX;
};

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
