/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file WorkGraphEvents.h
 * @brief Event definitions for monitoring WorkGraph execution - your window into the workflow
 *
 * This file contains all the event types that WorkGraph can emit during execution.
 * Subscribe to these events to build monitoring tools, debuggers, visualizers, or
 * just to understand what's happening inside your parallel workflows.
 */

#pragma once

#include <chrono>
#include <exception>
#include <string>

#include "WorkGraphTypes.h"

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

/**
 * @brief The mother of all WorkGraph events - timestamp and source
 *
 * Every event carries these two essential pieces of information: when it happened
 * and which graph it came from. The timestamp uses steady_clock for reliable
 * duration measurements even if the system clock changes.
 *
 * Minimal overhead design.
 *
 * @code
 * // All events inherit from this, so you can handle them generically
 * eventBus->subscribe<WorkGraphEvent>([](const WorkGraphEvent& event) {
 *     auto elapsed = std::chrono::steady_clock::now() - event.timestamp;
 *     LOG_DEBUG("Event from graph {} occurred {}ms ago",
 *               event.graph,
 *               std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
 * });
 * @endcode
 */
struct WorkGraphEvent
{
    std::chrono::steady_clock::time_point timestamp;  ///< When this event was created
    const WorkGraph* graph;                           ///< Which graph emitted this event

    explicit WorkGraphEvent(const WorkGraph* g) : timestamp(std::chrono::steady_clock::now()), graph(g) {}
};

/**
 * @brief The workhorse event - tracks every state transition in your workflow
 *
 * This is THE event for understanding execution flow. Every time a node changes
 * state (Pending→Ready, Ready→Scheduled, etc.), this event fires. Perfect for
 * building state diagrams, progress trackers, or debugging stuck workflows.
 *
 * @code
 * eventBus->subscribe<NodeStateChangedEvent>([](const auto& event) {
 *     LOG_INFO("Node {} transitioned from {} to {}",
 *              event.node.getData()->name,
 *              nodeStateToString(event.oldState),
 *              nodeStateToString(event.newState));
 *
 *     // Track progress
 *     if (event.newState == NodeState::Completed) {
 *         updateProgressBar();
 *     }
 * });
 * @endcode
 */
struct NodeStateChangedEvent : WorkGraphEvent
{
    NodeHandle node;     ///< The node that changed state
    NodeState oldState;  ///< What it was
    NodeState newState;  ///< What it is now

    NodeStateChangedEvent(const WorkGraph* g, NodeHandle n, NodeState from, NodeState to)
        : WorkGraphEvent(g), node(n), oldState(from), newState(to) {}
};

/**
 * @brief Fired when a new task joins your workflow
 *
 * Useful for dynamic graph visualization or tracking graph construction.
 * Note this fires immediately when addNode() is called, before any dependencies
 * are established.
 *
 * @code
 * eventBus->subscribe<NodeAddedEvent>([&nodeCount](const auto& event) {
 *     nodeCount++;
 *     LOG_DEBUG("Graph now has {} nodes", nodeCount);
 * });
 * @endcode
 */
struct NodeAddedEvent : WorkGraphEvent
{
    NodeHandle node;  ///< The newly added node

    NodeAddedEvent(const WorkGraph* g, NodeHandle n) : WorkGraphEvent(g), node(n) {}
};

/**
 * @brief Fired when you wire two nodes together
 *
 * This event captures the moment a dependency is established. The 'from' node
 * must complete before the 'to' node can run. Great for building visual
 * representations of your workflow structure.
 *
 * @code
 * // Build a dependency graph visualization
 * eventBus->subscribe<DependencyAddedEvent>([&graphViz](const auto& event) {
 *     graphViz.addEdge(
 *         event.from.getData()->name,
 *         event.to.getData()->name
 *     );
 * });
 * @endcode
 */
struct DependencyAddedEvent : WorkGraphEvent
{
    NodeHandle from;  ///< The prerequisite node (parent)
    NodeHandle to;    ///< The dependent node (child)

    DependencyAddedEvent(const WorkGraph* g, NodeHandle f, NodeHandle t) : WorkGraphEvent(g), from(f), to(t) {}
};

/**
 * @brief Success! A node finished without throwing
 *
 * This is your "mission accomplished" notification. The node ran to completion
 * without exceptions. Includes execution time for performance analysis - though
 * this might be zero if timing wasn't tracked.
 *
 * @code
 * // Performance monitoring
 * eventBus->subscribe<NodeCompletedEvent>([](const auto& event) {
 *     auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
 *         event.executionTime).count();
 *     LOG_INFO("Node {} completed in {}ms",
 *              event.node.getData()->name, ms);
 *
 *     if (ms > 1000) {
 *         LOG_WARN("Slow node detected!");
 *     }
 * });
 * @endcode
 */
struct NodeCompletedEvent : WorkGraphEvent
{
    NodeHandle node;                                    ///< The successful node
    std::chrono::steady_clock::duration executionTime;  ///< How long it took

    NodeCompletedEvent(const WorkGraph* g, NodeHandle n, std::chrono::steady_clock::duration duration = {})
        : WorkGraphEvent(g), node(n), executionTime(duration) {}
};

/**
 * @brief Uh oh - a node threw an exception
 *
 * When a node's work function throws, this event captures the failure. The
 * exception is preserved as exception_ptr so you can rethrow it for debugging
 * or log the details. Remember: node failure triggers cancellation of all
 * dependent nodes!
 *
 * @code
 * eventBus->subscribe<NodeFailedEvent>([](const auto& event) {
 *     LOG_ERROR("Node {} failed!", event.node.getData()->name);
 *
 *     if (event.exception) {
 *         try {
 *             std::rethrow_exception(event.exception);
 *         } catch (const std::exception& e) {
 *             LOG_ERROR("  Reason: {}", e.what());
 *         } catch (...) {
 *             LOG_ERROR("  Reason: Unknown exception");
 *         }
 *     }
 * });
 * @endcode
 */
struct NodeFailedEvent : WorkGraphEvent
{
    NodeHandle node;               ///< The node that failed
    std::exception_ptr exception;  ///< What was thrown (may be null)

    NodeFailedEvent(const WorkGraph* g, NodeHandle n, std::exception_ptr ex = nullptr)
        : WorkGraphEvent(g), node(n), exception(ex) {}
};

/**
 * @brief A node was cancelled due to upstream failure
 *
 * When a parent node fails, all its descendants get cancelled since their inputs
 * are invalid. This event tells you which nodes were skipped and why. The
 * failedParent tells you which upstream failure caused this cancellation.
 *
 * @code
 * eventBus->subscribe<NodeCancelledEvent>([](const auto& event) {
 *     LOG_WARN("Node {} cancelled due to failure of {}",
 *              event.node.getData()->name,
 *              event.failedParent.valid() ?
 *                  event.failedParent.getData()->name : "unknown");
 * });
 * @endcode
 */
struct NodeCancelledEvent : WorkGraphEvent
{
    NodeHandle node;          ///< The cancelled node
    NodeHandle failedParent;  ///< Which parent's failure caused this (may be invalid)

    NodeCancelledEvent(const WorkGraph* g, NodeHandle n, NodeHandle parent = NodeHandle())
        : WorkGraphEvent(g), node(n), failedParent(parent) {}
};

/**
 * @brief All dependencies satisfied - this node is ready to rock!
 *
 * Fired when a node transitions from Pending to Ready. This means all its
 * parent nodes have completed successfully and it's eligible for scheduling.
 * The actual scheduling might be delayed if the work queue is full.
 *
 * @code
 * // Track scheduling latency
 * std::unordered_map<NodeHandle, TimePoint> readyTimes;
 *
 * eventBus->subscribe<NodeReadyEvent>([&readyTimes](const auto& event) {
 *     readyTimes[event.node] = std::chrono::steady_clock::now();
 * });
 *
 * eventBus->subscribe<NodeScheduledEvent>([&readyTimes](const auto& event) {
 *     auto it = readyTimes.find(event.node);
 *     if (it != readyTimes.end()) {
 *         auto delay = std::chrono::steady_clock::now() - it->second;
 *         LOG_DEBUG("Scheduling delay: {}μs",
 *                   std::chrono::duration_cast<std::chrono::microseconds>(delay).count());
 *     }
 * });
 * @endcode
 */
struct NodeReadyEvent : WorkGraphEvent
{
    NodeHandle node;  ///< The node that's ready to execute

    NodeReadyEvent(const WorkGraph* g, NodeHandle n) : WorkGraphEvent(g), node(n) {}
};

/**
 * @brief Node has been submitted to the thread pool
 *
 * This fires when a ready node is successfully scheduled into the WorkContractGroup.
 * The node is now in the work queue waiting for a thread to pick it up. Next stop:
 * NodeExecutingEvent when a thread actually starts running it.
 *
 * @code
 * // Monitor queue depth
 * std::atomic<int> queuedNodes{0};
 *
 * eventBus->subscribe<NodeScheduledEvent>([&queuedNodes](const auto& event) {
 *     queuedNodes++;
 *     LOG_DEBUG("Work queue depth: {}", queuedNodes.load());
 * });
 *
 * eventBus->subscribe<NodeExecutingEvent>([&queuedNodes](const auto& event) {
 *     queuedNodes--;
 * });
 * @endcode
 */
struct NodeScheduledEvent : WorkGraphEvent
{
    NodeHandle node;  ///< The node that was queued

    NodeScheduledEvent(const WorkGraph* g, NodeHandle n) : WorkGraphEvent(g), node(n) {}
};

/**
 * @brief A thread has started running this node's work
 *
 * The moment of truth - a worker thread has dequeued this node and is about to
 * call its work function. Includes the thread ID for tracking thread utilization
 * and debugging thread-related issues.
 *
 * @code
 * // Thread utilization tracking
 * std::unordered_map<size_t, std::string> threadWork;
 *
 * eventBus->subscribe<NodeExecutingEvent>([&threadWork](const auto& event) {
 *     threadWork[event.threadId] = event.node.getData()->name;
 *     LOG_DEBUG("Thread {} executing: {}",
 *               event.threadId, event.node.getData()->name);
 * });
 * @endcode
 */
struct NodeExecutingEvent : WorkGraphEvent
{
    NodeHandle node;  ///< The node being executed
    size_t threadId;  ///< Which worker thread is running it

    NodeExecutingEvent(const WorkGraph* g, NodeHandle n, size_t tid = 0) : WorkGraphEvent(g), node(n), threadId(tid) {}
};

/**
 * @brief Work queue is full - this node has to wait
 *
 * When the WorkContractGroup is at capacity, ready nodes get deferred instead
 * of dropped. This event lets you monitor back-pressure in your system. High
 * deferral rates suggest you need more worker threads or smaller work items.
 *
 * @code
 * eventBus->subscribe<NodeDeferredEvent>([](const auto& event) {
 *     LOG_WARN("Node {} deferred - queue depth: {}",
 *              event.node.getData()->name, event.queueDepth);
 *
 *     if (event.queueDepth > 100) {
 *         LOG_ERROR("Severe backlog detected!");
 *     }
 * });
 * @endcode
 */
struct NodeDeferredEvent : WorkGraphEvent
{
    NodeHandle node;    ///< The node that couldn't be scheduled
    size_t queueDepth;  ///< How many nodes are waiting in deferred queue

    NodeDeferredEvent(const WorkGraph* g, NodeHandle n, size_t depth) : WorkGraphEvent(g), node(n), queueDepth(depth) {}
};

/**
 * @brief The starting gun - workflow execution begins!
 *
 * Fired when execute() is called. Gives you the big picture: how many total
 * nodes need to run and how many root nodes (no dependencies) are kicking
 * things off. Perfect for initializing progress tracking.
 *
 * @code
 * eventBus->subscribe<GraphExecutionStartedEvent>([](const auto& event) {
 *     LOG_INFO("Starting workflow with {} nodes ({} roots)",
 *              event.totalNodes, event.rootNodes);
 *     initProgressBar(event.totalNodes);
 * });
 * @endcode
 */
struct GraphExecutionStartedEvent : WorkGraphEvent
{
    size_t totalNodes;  ///< How many nodes in the entire graph
    size_t rootNodes;   ///< How many have no dependencies

    GraphExecutionStartedEvent(const WorkGraph* g, size_t total, size_t roots)
        : WorkGraphEvent(g), totalNodes(total), rootNodes(roots) {}
};

/**
 * @brief The finish line - all nodes have reached terminal states
 *
 * This fires when the entire graph is done, whether successfully or not.
 * The stats snapshot gives you the complete picture: successes, failures,
 * cancellations, and performance metrics. This is your post-mortem data.
 *
 * @code
 * eventBus->subscribe<GraphExecutionCompletedEvent>([](const auto& event) {
 *     const auto& stats = event.stats;
 *     LOG_INFO("Workflow complete: {} succeeded, {} failed, {} cancelled",
 *              stats.completedNodes, stats.failedNodes, stats.cancelledNodes);
 *
 *     auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
 *         stats.totalExecutionTime).count();
 *     LOG_INFO("Total execution time: {}s", seconds);
 *
 *     if (stats.failedNodes > 0) {
 *         LOG_ERROR("Workflow had failures!");
 *     }
 * });
 * @endcode
 */
struct GraphExecutionCompletedEvent : WorkGraphEvent
{
    WorkGraphStats::Snapshot stats;  ///< Final statistics for the run

    GraphExecutionCompletedEvent(const WorkGraph* g, const WorkGraphStats::Snapshot& s) : WorkGraphEvent(g), stats(s) {}
};

/**
 * @brief One step closer - a parent completed and child's dependency count dropped
 *
 * This granular event fires each time a parent node completes and decrements
 * a child's dependency counter. When remainingDependencies reaches zero, the
 * child becomes ready. Useful for understanding the cascade of execution.
 *
 * @code
 * eventBus->subscribe<DependencyResolvedEvent>([](const auto& event) {
 *     LOG_DEBUG("{} completed, {} now has {} dependencies left",
 *               event.from.getData()->name,
 *               event.to.getData()->name,
 *               event.remainingDependencies);
 *
 *     if (event.remainingDependencies == 0) {
 *         LOG_INFO("{} is now ready!", event.to.getData()->name);
 *     }
 * });
 * @endcode
 */
struct DependencyResolvedEvent : WorkGraphEvent
{
    NodeHandle from;                 ///< Parent that just completed
    NodeHandle to;                   ///< Child being notified
    uint32_t remainingDependencies;  ///< How many more parents must complete

    DependencyResolvedEvent(const WorkGraph* g, NodeHandle parent, NodeHandle child, uint32_t remaining)
        : WorkGraphEvent(g), from(parent), to(child), remainingDependencies(remaining) {}
};

/**
 * @brief Periodic health check - current graph statistics
 *
 * Some implementations fire this periodically during execution to provide
 * real-time monitoring without needing to poll getStats(). The frequency
 * depends on the implementation. Useful for dashboards and progress bars.
 *
 * @code
 * // Real-time progress monitoring
 * eventBus->subscribe<GraphStatsEvent>([](const auto& event) {
 *     const auto& stats = event.stats;
 *     float progress = (float)(stats.completedNodes + stats.failedNodes +
 *                             stats.cancelledNodes) / stats.totalNodes * 100;
 *
 *     updateProgressBar(progress);
 *     updateStatusText("Running: {}, Queued: {}, Complete: {}",
 *                      stats.executingNodes,
 *                      stats.scheduledNodes,
 *                      stats.completedNodes);
 * });
 * @endcode
 */
struct GraphStatsEvent : WorkGraphEvent
{
    WorkGraphStats::Snapshot stats;  ///< Current statistics snapshot

    GraphStatsEvent(const WorkGraph* g, const WorkGraphStats::Snapshot& s) : WorkGraphEvent(g), stats(s) {}
};

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
