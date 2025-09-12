/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file WorkGraph.h
 * @brief Dependency-based work execution system with automatic scheduling
 * 
 * This file contains the WorkGraph class, which manages work nodes with dependency
 * relationships. It automatically schedules work when dependencies are satisfied
 * and provides synchronization primitives for complex parallel workflows.
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <variant>
#include <optional>
#include "WorkContractGroup.h"
#include "WorkContractHandle.h"
#include "../Graph/DirectedAcyclicGraph.h"
#include "../Graph/AcyclicNodeHandle.h"
#include "../Debug/Debug.h"
#include "WorkGraphTypes.h"
#include "../Core/EventBus.h"
#include <memory>

namespace EntropyEngine {
namespace Core {
namespace Concurrency {

    // Forward declarations
    class NodeStateManager;
    class NodeScheduler;

    /**
     * @brief The atomic unit of work in a dependency graph with self-managing execution timing
     * 
     * WorkGraphNode represents a single task within a dependency graph. Each node encapsulates
     * a work function along with the necessary state management to ensure execution occurs
     * precisely when all dependencies are satisfied. The node maintains complete awareness
     * of its position within the execution hierarchy.
     * 
     * The node tracks:
     * - Its current state (pending, executing, completed, yielded, etc.)
     * - How many parents need to finish before it can run
     * - Whether any parent failed (so it should be cancelled)
     * - Its work contract handle for execution
     * - Reschedule count for yieldable nodes
     * 
     * All state transitions use atomic operations to ensure thread-safe updates when multiple
     * parent nodes complete concurrently. This design enables safe concurrent dependency
     * count decrements without locks.
     * 
     * @code
     * // Nodes are created internally by WorkGraph::addNode()
     * // You interact with them through NodeHandle, not directly
     * auto node = graph.addNode([]{ 
     *     processData(); 
     * }, "data-processor");
     * 
     * // Or create a yieldable node
     * auto yieldNode = graph.addYieldableNode([]() -> WorkResult {
     *     if (!ready()) return WorkResult::Yield;
     *     process();
     *     return WorkResult::Complete;
     * }, "yielder");
     * @endcode
     */
    struct WorkGraphNode {
        /// Atomic state management (replaces completed/cancelled flags)
        std::atomic<NodeState> state{NodeState::Pending};
        
        /// Work function to execute - now supports both void and WorkResult returns
        std::variant<std::function<void()>, YieldableWorkFunction> work;
        
        /// Handle to the work contract (when scheduled)
        WorkContractHandle handle;
        
        /// Number of uncompleted dependencies
        std::atomic<uint32_t> pendingDependencies{0};
        
        /// Number of failed parents (for optimized parent checking)
        std::atomic<uint32_t> failedParentCount{0};
        
        /// Track if completion has been processed to prevent double processing
        std::atomic<bool> completionProcessed{false};
        
        /// Debug name for the node
        std::string name;
        
        /// User data pointer for custom context
        void* userData = nullptr;
        
        /// Execution type for this node (main thread or any thread)
        ExecutionType executionType = ExecutionType::AnyThread;
        
        /// Reschedule tracking for yieldable nodes
        std::atomic<uint32_t> rescheduleCount{0};
        
        /// Optional maximum reschedule limit
        std::optional<uint32_t> maxReschedules;
        
        /// Is this a yieldable node?
        bool isYieldable = false;
        
        WorkGraphNode() = default;
        
        // Constructor for legacy void() work functions
        WorkGraphNode(std::function<void()> w, const std::string& n, ExecutionType execType = ExecutionType::AnyThread) 
            : work(std::move(w)), name(n), executionType(execType), isYieldable(false) {}
        
        // Constructor for yieldable work functions
        WorkGraphNode(YieldableWorkFunction w, const std::string& n, ExecutionType execType = ExecutionType::AnyThread) 
            : work(std::move(w)), name(n), executionType(execType), isYieldable(true) {}
        
        // Move constructor
        WorkGraphNode(WorkGraphNode&& other) noexcept
            : state(other.state.load())
            , work(std::move(other.work))
            , handle(std::move(other.handle))
            , pendingDependencies(other.pendingDependencies.load())
            , failedParentCount(other.failedParentCount.load())
            , completionProcessed(other.completionProcessed.load())
            , name(std::move(other.name))
            , userData(other.userData)
            , executionType(other.executionType)
            , rescheduleCount(other.rescheduleCount.load())
            , maxReschedules(other.maxReschedules)
            , isYieldable(other.isYieldable) {
            other.userData = nullptr;
        }
        
        // Move assignment
        WorkGraphNode& operator=(WorkGraphNode&& other) noexcept {
            if (this != &other) {
                state.store(other.state.load());
                work = std::move(other.work);
                handle = std::move(other.handle);
                pendingDependencies.store(other.pendingDependencies.load());
                failedParentCount.store(other.failedParentCount.load());
                completionProcessed.store(other.completionProcessed.load());
                name = std::move(other.name);
                userData = other.userData;
                executionType = other.executionType;
                rescheduleCount.store(other.rescheduleCount.load());
                maxReschedules = other.maxReschedules;
                isYieldable = other.isYieldable;
                other.userData = nullptr;
            }
            return *this;
        }
        
        // Delete copy operations
        WorkGraphNode(const WorkGraphNode&) = delete;
        WorkGraphNode& operator=(const WorkGraphNode&) = delete;
    };

    /**
     * @brief Orchestrates complex parallel workflows with automatic dependency management
     * 
     * WorkGraph provides high-level workflow management for parallel task execution. It accepts
     * task definitions with dependency relationships and automatically determines optimal
     * execution order. As tasks complete, the system triggers dependent tasks in cascade,
     * ensuring correct execution flow through complex dependency chains.
     * 
     * Bridges the gap between low-level work execution (WorkContractGroup) and
     * high-level workflow requirements. While WorkContractGroup provides raw execution
     * capabilities, WorkGraph adds intelligent scheduling based on dependency relationships.
     * 
     * Key features:
     * - Automatic dependency resolution without manual scheduling
     * - Dynamic graph construction during execution
     * - Failure propagation to cancel dependent tasks
     * - Thread-safe operations for concurrent modifications
     * - Zero-copy integration with existing WorkContractGroup
     * - Main thread execution support for UI and render operations
     * 
     * Common applications:
     * - Build systems (compile → link → package)
     * - Data pipelines (load → transform → analyze → save)
     * - Game asset processing (texture → compress → pack)
     * - Mixed UI/background workflows (process → update UI → save)
     * 
     * Complexity characteristics:
     * - Dependency tracking: O(1) atomic operations
     * - Completion cascade: O(children) per completed node
     * 
     * @code
     * // Mixed execution pipeline with UI updates
     * WorkContractGroup group(1024);
     * WorkService service(2);  // 2 worker threads
     * service.addWorkContractGroup(&group);
     * 
     * WorkGraph graph(&group);
     * 
     * // Background data processing
     * auto load = graph.addNode([]{ 
     *     auto data = loadFromDisk();
     *     processData(data);
     * }, "loader");
     * 
     * // Main thread UI update
     * auto updateUI = graph.addNode([]{
     *     progressBar.setValue(50);
     *     statusLabel.setText("Processing...");
     * }, "ui-update", nullptr, ExecutionType::MainThread);
     * 
     * // More background work
     * auto save = graph.addNode([]{
     *     auto data = getProcessedData();
     *     saveToDisk(data);
     * }, "saver");
     * 
     * // Wire up dependencies - UI update after load, save after UI
     * graph.addDependency(load, updateUI);
     * graph.addDependency(updateUI, save);
     * 
     * // Start execution
     * graph.execute();
     * service.start();
     * 
     * // Main thread pump (in your event loop)
     * while (!graph.isComplete()) {
     *     // Process main thread work
     *     group.executeMainThreadWork(5);  // Max 5 per frame
     *     
     *     // Handle UI events
     *     processEvents();
     *     renderFrame();
     * }
     * @endcode
     */
    class WorkGraph : public Debug::Named {
    private:
        /// RAII guard for tracking active callbacks
        struct CallbackGuard {
            WorkGraph* graph;
            CallbackGuard(WorkGraph* g) : graph(g) {
                graph->_activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
            }
            ~CallbackGuard() {
                if (graph->_activeCallbacks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> lock(graph->_waitMutex);
                    graph->_shutdownCondition.notify_all();
                }
            }
        };
        
        Graph::DirectedAcyclicGraph<WorkGraphNode> _graph;                   ///< Internal DAG implementation
        
        WorkContractGroup* _workContractGroup;                               ///< External work executor
        
        WorkContractGroup::CapacityCallback _capacityCallbackIt;             ///< Capacity callback handle
        
        WorkGraphConfig _config;                                              ///< Graph configuration
        
        std::unique_ptr<Core::EventBus> _eventBus;                          ///< Event system
        std::unique_ptr<NodeStateManager> _stateManager;                    ///< State tracker
        std::unique_ptr<NodeScheduler> _scheduler;                          ///< Node scheduler
        
        mutable std::shared_mutex _graphMutex;                              ///< Graph structure protection
        
        std::atomic<bool> _executionStarted{false};                         ///< Execution started flag
        
        std::atomic<uint32_t> _pendingNodes{0};                             ///< Pending node count
        
        std::atomic<uint32_t> _droppedNodes{0};                             ///< Dropped node count
        
        std::atomic<uint32_t> _completedNodes{0};                           ///< Completed node count
        
        std::atomic<uint32_t> _failedNodes{0};                              ///< Failed node count
        
        /// Cache of valid node handles for efficient access
        std::vector<Graph::AcyclicNodeHandle<WorkGraphNode>> _nodeHandles;  ///< Pre-allocated for performance
        
        /// Callback for node completion (for testing/debugging)
        std::function<void(Graph::AcyclicNodeHandle<WorkGraphNode>)> _onNodeComplete;  ///< Optional completion hook
        
        /// Synchronization for wait() - more efficient than busy-waiting
        mutable std::mutex _waitMutex;                                      ///< Protects wait condition
        mutable std::condition_variable _waitCondition;                     ///< Signaled when nodes complete
        
        /// Safety flag to prevent callbacks after destruction
        mutable std::atomic<bool> _destroyed{false};                        ///< Set true in destructor
        
        /// Number of active callbacks (for safe destruction)
        mutable std::atomic<uint32_t> _activeCallbacks{0};                  ///< Tracked by CallbackGuard
        
        /// Condition variable for waiting on active callbacks
        mutable std::condition_variable _shutdownCondition;                 ///< Destructor waits on this
        
        /// Suspension state - prevents scheduling new nodes
        std::atomic<bool> _suspended{false};                                ///< True when graph is suspended
        
    public:
        using NodeHandle = Graph::AcyclicNodeHandle<WorkGraphNode>;
        
        /**
         * @brief What you get back from wait() - the final score of your graph execution
         * 
         * This tells you how your workflow went: did everything finish? Did some tasks
         * fail? Were any dropped due to capacity issues? It's like a report card for
         * your parallel execution.
         * 
         * @code
         * auto result = graph.wait();
         * if (result.allCompleted) {
         *     LOG_INFO("Perfect run! All {} nodes completed", result.completedCount);
         * } else {
         *     LOG_WARN("Issues detected: {} failed, {} dropped", 
         *              result.failedCount, result.droppedCount);
         * }
         * @endcode
         */
        struct WaitResult {
            bool allCompleted = false;      ///< True only if every single node succeeded
            uint32_t droppedCount = 0;      ///< Nodes we couldn't schedule (queue overflow)
            uint32_t failedCount = 0;       ///< Nodes that threw exceptions
            uint32_t completedCount = 0;    ///< Nodes that ran successfully
        };
        
        /**
         * @brief Creates a work graph backed by your thread pool
         * 
         * The graph doesn't own the WorkContractGroup - just uses it to schedule work.
         * Multiple graphs can share the same thread pool.
         * 
         * @param workContractGroup Your thread pool for executing work (must outlive the graph)
         * 
         * @code
         * // Typical setup
         * WorkContractGroup threadPool(1024);  // Shared thread pool
         * WorkGraph pipeline1(&threadPool);    // Asset processing pipeline  
         * WorkGraph pipeline2(&threadPool);    // Data analysis pipeline
         * // Both pipelines share the same threads!
         * @endcode
         */
        explicit WorkGraph(WorkContractGroup* workContractGroup);
        
        /**
         * @brief Creates a work graph with custom behavior options
         * 
         * Use for advanced features like events, state management, or custom allocation.
         * 
         * @param workContractGroup Your thread pool for executing work
         * @param config Tuning knobs and feature flags
         * 
         * @code
         * // Example: Graph with event notifications for monitoring
         * WorkGraphConfig config;
         * config.enableEvents = true;
         * config.expectedNodeCount = 1000;  // Pre-allocate for performance
         * config.maxDeferredNodes = 100;    // Limit queue size
         * 
         * WorkGraph monitoredGraph(&threadPool, config);
         * 
         * // Now you can subscribe to events
         * monitoredGraph.getEventBus()->subscribe<NodeCompletedEvent>(
         *     [](const NodeCompletedEvent& e) {
         *         LOG_INFO("Node completed in {}ms", 
         *                  chrono::duration_cast<chrono::milliseconds>(e.executionTime).count());
         *     });
         * @endcode
         */
        WorkGraph(WorkContractGroup* workContractGroup, const WorkGraphConfig& config);
        
        /**
         * @brief Cleans up the graph and ensures all callbacks complete
         * 
         * Waits for active callbacks before destroying. Safe to destroy with pending
         * work - it continues executing in the WorkContractGroup.
         */
        ~WorkGraph();
        
        /**
         * @brief Adds a yieldable task that can suspend and resume execution
         * 
         * Creates a node that can yield control back to the scheduler and be
         * rescheduled later. Perfect for polling operations, staged processing,
         * or any task that needs to wait without blocking a thread.
         * 
         * @param work Yieldable function returning WorkResult
         * @param name Human-readable name for debugging
         * @param userData Your own context pointer
         * @param executionType Where to run: AnyThread or MainThread
         * @param maxReschedules Optional limit on reschedules (prevent infinite loops)
         * @return Handle to reference this node
         * 
         * @code
         * // Polling task that yields until ready
         * auto poller = graph.addYieldableNode([]() -> WorkResult {
         *     if (!dataReady()) {
         *         return WorkResult::Yield;  // Try again later
         *     }
         *     processData();
         *     return WorkResult::Complete;
         * }, "data-poller");
         * 
         * // Staged processing with yield between stages
         * int stage = 0;
         * auto staged = graph.addYieldableNode([&stage]() -> WorkResult {
         *     switch (stage++) {
         *         case 0: doStage1(); return WorkResult::Yield;
         *         case 1: doStage2(); return WorkResult::Yield;
         *         case 2: doStage3(); return WorkResult::Complete;
         *         default: return WorkResult::Complete;
         *     }
         * }, "staged-processor", nullptr, ExecutionType::AnyThread, 10);
         * @endcode
         */
        NodeHandle addYieldableNode(YieldableWorkFunction work,
                                   const std::string& name = "",
                                   void* userData = nullptr,
                                   ExecutionType executionType = ExecutionType::AnyThread,
                                   std::optional<uint32_t> maxReschedules = std::nullopt);
        
        /**
         * @brief Adds a task to your workflow - it won't run until its time comes
         * 
         * Creates a node that waits for dependencies before running. Thread-safe -
         * can add nodes while graph executes. Use ExecutionType::MainThread for
         * UI updates or other main-thread-only operations.
         * 
         * @param work Your task - lambda, function, or any callable
         * @param name Human-readable name for debugging
         * @param userData Your own context pointer
         * @param executionType Where to run: AnyThread (worker pool) or MainThread
         * @return Handle to reference this node
         * 
         * @code
         * // Simple task
         * auto task = graph.addNode([]{ 
         *     doSomeWork(); 
         * }, "worker-1");
         * 
         * // Main thread task
         * auto uiUpdate = graph.addNode([]{ 
         *     updateUI(); 
         * }, "ui-updater", nullptr, ExecutionType::MainThread);
         * 
         * // Task with captures
         * std::string filename = "data.txt";
         * auto loader = graph.addNode([filename]{ 
         *     loadFile(filename); 
         * }, "file-loader");
         * 
         * // Task with user data
         * auto* context = new ProcessContext();
         * auto processor = graph.addNode(
         *     [context]{ context->process(); }, 
         *     "processor", 
         *     context  // Attach as user data
         * );
         * @endcode
         */
        NodeHandle addNode(std::function<void()> work, 
                          const std::string& name = "",
                          void* userData = nullptr,
                          ExecutionType executionType = ExecutionType::AnyThread);
        
        /**
         * @brief Wire up your workflow - tell nodes who they're waiting for
         * 
         * Defines execution order: "to" waits for "from" to finish. If "from" fails,
         * "to" is cancelled. Prevents cycles. Thread-safe.
         * 
         * @param from The prerequisite task
         * @param to The dependent task
         * @throws std::invalid_argument if this would create a cycle
         * @throws std::runtime_error if nodes invalid or completed
         * 
         * @code
         * // Linear pipeline: A → B → C
         * auto A = graph.addNode([]{ stepA(); }, "A");
         * auto B = graph.addNode([]{ stepB(); }, "B");
         * auto C = graph.addNode([]{ stepC(); }, "C");
         * graph.addDependency(A, B);  // B waits for A
         * graph.addDependency(B, C);  // C waits for B
         * 
         * // Fan-out: A → {B, C, D}
         * auto A = graph.addNode([]{ generateData(); }, "generator");
         * auto B = graph.addNode([]{ process1(); }, "proc1");
         * auto C = graph.addNode([]{ process2(); }, "proc2");
         * auto D = graph.addNode([]{ process3(); }, "proc3");
         * graph.addDependency(A, B);  // All three process
         * graph.addDependency(A, C);  // the same data
         * graph.addDependency(A, D);  // in parallel
         * 
         * // Fan-in: {A, B, C} → D
         * auto D = graph.addNode([]{ mergeResults(); }, "merger");
         * graph.addDependency(A, D);  // D waits for
         * graph.addDependency(B, D);  // all three
         * graph.addDependency(C, D);  // to complete
         * @endcode
         */
        void addDependency(NodeHandle from, NodeHandle to);
        
        /**
         * @brief Kicks off your workflow by scheduling all nodes that have no dependencies
         * 
         * Finds root nodes and schedules them. Called automatically by execute().
         * 
         * @return Number of root nodes that were scheduled
         * 
         * @code
         * // Manual execution control
         * graph.addNode([]{ step1(); }, "step1");
         * graph.addNode([]{ step2(); }, "step2"); 
         * // Both are roots since no dependencies were added
         * 
         * size_t roots = graph.scheduleRoots();  // Returns 2
         * LOG_INFO("Started {} independent tasks", roots);
         * @endcode
         */
        size_t scheduleRoots();
        
        /**
         * @brief Lights the fuse on your workflow - starts the cascade of execution
         * 
         * Finds and schedules root nodes. Safe to call multiple times. Thread-safe
         * with dynamic modifications.
         * 
         * @code
         * // Fire and forget
         * graph.execute();
         * // Graph is now running in the background
         * 
         * // You can even add more work while it runs!
         * auto newNode = graph.addNode([]{ lateWork(); });
         * graph.addDependency(existingNode, newNode);
         * // newNode will execute when existingNode completes
         * @endcode
         */
        void execute();
        
        /**
         * @brief Suspends graph execution - no new nodes will be scheduled
         * 
         * Currently executing nodes will complete, but no new nodes will be
         * scheduled (including yielded nodes trying to reschedule). The graph
         * remains suspended until resume() is called.
         * 
         * Thread-safe. Can be called while graph is executing.
         * 
         * @code
         * graph.execute();
         * // ... some time later
         * graph.suspend();  // Pause execution
         * // ... do something else
         * graph.resume();   // Continue where we left off
         * @endcode
         */
        void suspend();
        
        /**
         * @brief Resumes graph execution after suspension
         * 
         * Allows scheduling to continue. Any nodes that became ready while
         * suspended will be scheduled. Yielded nodes waiting to reschedule
         * will also continue.
         * 
         * Thread-safe. Safe to call even if not suspended.
         * 
         * @code
         * if (needToPause) {
         *     graph.suspend();
         *     handleHighPriorityWork();
         *     graph.resume();
         * }
         * @endcode
         */
        void resume();
        
        /**
         * @brief Checks if the graph is currently suspended
         * 
         * @return true if suspend() was called and resume() hasn't been called yet
         */
        bool isSuspended() const noexcept { return _suspended.load(std::memory_order_acquire); }
        
        /**
         * @brief Blocks until your entire workflow finishes - success or failure
         * 
         * Synchronization point using condition variables. Returns execution summary.
         * Thread-safe.
         * 
         * @return Execution summary with success/failure counts
         * 
         * @code
         * graph.execute();
         * // Do other work while graph runs...
         * 
         * auto result = graph.wait();
         * if (result.allCompleted) {
         *     LOG_INFO("Workflow completed successfully!");
         * } else if (result.failedCount > 0) {
         *     LOG_ERROR("Workflow had {} failures", result.failedCount);
         *     // Check which nodes failed for debugging
         * } else if (result.droppedCount > 0) {
         *     LOG_WARN("Dropped {} nodes due to capacity", result.droppedCount);
         *     // Maybe increase WorkContractGroup size?
         * }
         * @endcode
         */
        WaitResult wait();
        
        /**
         * @brief Quick non-blocking check if your workflow is done
         * 
         * Returns true when all nodes reached terminal state. Perfect for polling
         * in game loops.
         * 
         * @return true if all nodes are done, false if work remains
         * 
         * @code
         * // In your game loop
         * if (!graph.isComplete()) {
         *     renderLoadingScreen();
         * } else {
         *     auto stats = graph.getStats();
         *     if (stats.failedNodes == 0) {
         *         proceedToNextLevel();
         *     } else {
         *         showErrorDialog();
         *     }
         * }
         * @endcode
         */
        bool isComplete() const;
        
        /**
         * @brief Manually drain the deferred queue when capacity becomes available
         * 
         * Schedules deferred nodes when capacity frees up. Usually automatic via
         * callbacks.
         * 
         * @return How many deferred nodes were successfully scheduled
         * 
         * @code
         * // After manually cancelling some work
         * workGroup.cancelSomeContracts();
         * size_t scheduled = graph.processDeferredNodes();
         * LOG_INFO("Scheduled {} previously deferred nodes", scheduled);
         * @endcode
         */
        size_t processDeferredNodes();
        
        /**
         * @brief Access the event system for monitoring graph execution
         * 
         * Lazy-initialized event bus if events enabled. Returns nullptr otherwise.
         * 
         * @return Event bus for subscriptions, or nullptr if events disabled
         * 
         * @code
         * // Subscribe to completion events
         * if (auto* bus = graph.getEventBus()) {
         *     bus->subscribe<NodeCompletedEvent>([](const auto& event) {
         *         LOG_INFO("Node completed: {} in {}ms", 
         *                  event.node.getData()->name,
         *                  duration_cast<milliseconds>(event.executionTime).count());
         *     });
         *     
         *     bus->subscribe<NodeFailedEvent>([](const auto& event) {
         *         LOG_ERROR("Node failed: {}", event.node.getData()->name);
         *         // Could rethrow the exception for debugging
         *     });
         * }
         * @endcode
         */
        Core::EventBus* getEventBus();
        
        /**
         * @brief Snapshot of your workflow's current state - how's it doing?
         * 
         * Returns consistent snapshot of all stats. Great for progress bars or
         * monitoring.
         * 
         * @return Complete statistics snapshot at this moment
         * 
         * @code
         * // Progress monitoring
         * auto stats = graph.getStats();
         * float progress = (float)stats.completedNodes / stats.totalNodes * 100;
         * LOG_INFO("Progress: {:.1f}% ({}/{} nodes)", 
         *          progress, stats.completedNodes, stats.totalNodes);
         * 
         * if (stats.failedNodes > 0) {
         *     LOG_WARN("Failures detected: {} nodes failed", stats.failedNodes);
         * }
         * @endcode
         */
        WorkGraphStats::Snapshot getStats() const;
        
        /**
         * @brief Access the configuration this graph was created with
         * @return The config struct passed to constructor (or defaults)
         */
        const WorkGraphConfig& getConfig() const { return _config; }
        
        /**
         * @brief Quick check of how much work remains
         * @return Nodes that haven't reached terminal state yet
         */
        uint32_t getPendingCount() const { 
            return _pendingNodes.load(std::memory_order_acquire); 
        }
        
        /**
         * @brief Install a hook that fires whenever a node finishes
         * 
         * Simple completion tracking. Called synchronously - keep it fast!
         * 
         * @param callback Function to call on each node completion
         * 
         * @code
         * // Simple progress tracker
         * std::atomic<int> completed{0};
         * graph.setNodeCompleteCallback([&completed](NodeHandle node) {
         *     int count = ++completed;
         *     if (count % 100 == 0) {
         *         LOG_INFO("Completed {} nodes", count);
         *     }
         * });
         * @endcode
         */
        void setNodeCompleteCallback(std::function<void(NodeHandle)> callback) {
            _onNodeComplete = std::move(callback);
        }
        
        /**
         * @brief Create a "join" node that waits for multiple parents - perfect for fan-in patterns
         * 
         * Convenience for creating a node that waits for multiple parents. Runs only
         * after ALL parents complete successfully.
         * 
         * @param parents All nodes that must complete first
         * @param work What to do after all parents finish
         * @param name Debug label for the continuation node
         * @param executionType Where this node should execute (default: AnyThread)
         * @return Handle to the newly created continuation node
         * 
         * @code
         * // Parallel processing with merge
         * auto part1 = graph.addNode([]{ processPart1(); });
         * auto part2 = graph.addNode([]{ processPart2(); });
         * auto part3 = graph.addNode([]{ processPart3(); });
         * 
         * // Single merge point
         * auto merge = graph.addContinuation(
         *     {part1, part2, part3},
         *     []{ mergeResults(); },
         *     "merger"
         * );
         * 
         * // Main thread UI update after merge
         * auto uiUpdate = graph.addContinuation(
         *     {merge},
         *     []{ updateUI(); },
         *     "ui-updater",
         *     ExecutionType::MainThread
         * );
         * @endcode
         */
        NodeHandle addContinuation(const std::vector<NodeHandle>& parents,
                                  std::function<void()> work,
                                  const std::string& name = "",
                                  ExecutionType executionType = ExecutionType::AnyThread);
        
        
        /**
         * @brief Test if a node handle still points to a real node
         * 
         * @param handle The handle to validate
         * @return true if the handle points to a valid node
         */
        bool isHandleValid(const NodeHandle& handle) const {
            return _graph.isHandleValid(handle);
        }
        
        /**
         * @brief Find all nodes that depend on this one
         * 
         * Returns direct children. Useful for debugging or visualization.
         * 
         * @param node The parent whose children you want
         * @return List of nodes that depend on the given node
         * 
         * @code
         * // Find what depends on a critical node
         * auto children = graph.getChildren(criticalNode);
         * LOG_INFO("Node has {} dependents", children.size());
         * for (auto& child : children) {
         *     LOG_INFO("  - {}", child.getData()->name);
         * }
         * @endcode
         */
        std::vector<NodeHandle> getChildren(const NodeHandle& node) const {
            return _graph.getChildren(node);
        }
        
    private:
        /**
         * @brief The domino effect handler - when one task finishes, what happens next?
         * 
         * Heart of dependency resolution. Decrements child dependency counts and
         * schedules ready nodes. Uses atomics for thread-safe concurrent updates.
         * Called automatically by work wrapper.
         * 
         * @param node The node that just finished executing successfully
         */
        void onNodeComplete(NodeHandle node);
        
        /**
         * @brief Takes a ready node and gets it running in the thread pool
         * 
         * Wraps work with dependency tracking and exception handling. Uses CAS
         * for exactly-once execution. Called only by execute() and onNodeComplete().
         * 
         * @param node The ready node to submit for execution
         * @return true if scheduled, false if already scheduled/completed
         */
        bool scheduleNode(NodeHandle node);
        
        /**
         * @brief Bumps up how many parents a node is waiting for
         * 
         * Called by addDependency(). Atomic to handle concurrent graph construction.
         * Private - users work through addDependency().
         * 
         * @param node The node that just got another parent to wait for
         */
        void incrementDependencies(NodeHandle node);
        
        /**
         * @brief Internal root scheduling - assumes you already hold the graph lock
         * 
         * Called by execute() with lock held. Scans for zero-dependency nodes.
         * Private - unsafe without proper locking.
         * 
         * @return How many root nodes were found and scheduled
         */
        size_t scheduleRootsLocked();
        
        
        /**
         * @brief Propagates failure through the graph - if parent fails, children can't run
         * 
         * Cascades cancellation transitively through dependent nodes. Private - only
         * triggered by onNodeFailed().
         * 
         * @param failedNode The node whose failure triggers the cascade
         */
        void cancelDependents(NodeHandle failedNode);
        
        /**
         * @brief Handles the bookkeeping when a node gets cancelled
         * 
         * Updates counters and fires events. Cancellation cascades to children.
         * Private - only triggered internally.
         * 
         * @param node The node that's being cancelled
         */
        void onNodeCancelled(NodeHandle node);
        
        /**
         * @brief Deals with the aftermath when a node's work function throws
         * 
         * Updates stats, fires events with exception details, cancels dependents.
         * Private - called by work wrapper.
         * 
         * @param node The node whose work function threw an exception
         */
        void onNodeFailed(NodeHandle node);
        
        /**
         * @brief Handles a node that has yielded execution
         * 
         * Transitions the node from Executing to Yielded state and reschedules it
         * for later execution. Checks reschedule limits to prevent infinite loops.
         * 
         * @param node The node that yielded
         */
        void onNodeYielded(NodeHandle node);
        
        /**
         * @brief Reschedules a yielded node for execution
         * 
         * Transitions the node from Yielded to Ready state and schedules it
         * again. Called after a node yields to give it another chance to run.
         * 
         * @param node The yielded node to reschedule
         */
        void rescheduleYieldedNode(NodeHandle node);
        
    };

} // namespace Concurrency
} // namespace Core
} // namespace EntropyEngine

