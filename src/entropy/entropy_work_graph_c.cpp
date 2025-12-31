/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include <cstring>
#include <new>
#include <string>

#include "../../include/entropy/entropy_work_graph.h"
#include "../Concurrency/WorkContractGroup.h"
#include "../Concurrency/WorkGraph.h"
#include "../Concurrency/WorkGraphTypes.h"

using namespace EntropyEngine::Core::Concurrency;

// ============================================================================
// Internal Helpers
// ============================================================================

namespace
{

// Centralized exception translation
void translate_exception(EntropyStatus* status) {
    if (!status) return;

    try {
        throw;  // Re-throw current exception
    } catch (const std::bad_alloc&) {
        *status = ENTROPY_ERR_NO_MEMORY;
    } catch (const std::invalid_argument&) {
        *status = ENTROPY_ERR_INVALID_ARG;
    } catch (const std::exception&) {
        *status = ENTROPY_ERR_UNKNOWN;
    } catch (...) {
        std::terminate();  // Unknown exception = programming bug
    }
}

// Safe cast from opaque handle to C++ object
inline WorkGraph* to_cpp(entropy_WorkGraph graph) {
    return reinterpret_cast<WorkGraph*>(graph);
}

// Safe cast from C++ object to opaque handle
inline entropy_WorkGraph to_c(WorkGraph* graph) {
    return reinterpret_cast<entropy_WorkGraph>(graph);
}

// Safe cast from opaque node handle to C++ handle
inline WorkGraph::NodeHandle* to_cpp_node(entropy_NodeHandle handle) {
    return reinterpret_cast<WorkGraph::NodeHandle*>(handle);
}

// Safe cast from C++ node handle to opaque handle
inline entropy_NodeHandle to_c_node(WorkGraph::NodeHandle* handle) {
    return reinterpret_cast<entropy_NodeHandle>(handle);
}

// Convert C ExecutionType to C++ enum
ExecutionType to_cpp_execution_type(EntropyExecutionType type) {
    switch (type) {
        case ENTROPY_EXEC_ANY_THREAD:
            return ExecutionType::AnyThread;
        case ENTROPY_EXEC_MAIN_THREAD:
            return ExecutionType::MainThread;
        default:
            return ExecutionType::AnyThread;
    }
}

// Convert C++ NodeState to C enum
EntropyNodeState to_c_node_state(NodeState state) {
    switch (state) {
        case NodeState::Pending:
            return ENTROPY_NODE_PENDING;
        case NodeState::Ready:
            return ENTROPY_NODE_READY;
        case NodeState::Scheduled:
            return ENTROPY_NODE_SCHEDULED;
        case NodeState::Executing:
            return ENTROPY_NODE_EXECUTING;
        case NodeState::Completed:
            return ENTROPY_NODE_COMPLETED;
        case NodeState::Failed:
            return ENTROPY_NODE_FAILED;
        case NodeState::Cancelled:
            return ENTROPY_NODE_CANCELLED;
        case NodeState::Yielded:
            return ENTROPY_NODE_YIELDED;
        default:
            return ENTROPY_NODE_PENDING;
    }
}

// Convert C++ WorkResult to C enum
EntropyWorkResult to_c_work_result(WorkResult result) {
    switch (result) {
        case WorkResult::Complete:
            return ENTROPY_WORK_COMPLETE;
        case WorkResult::Yield:
            return ENTROPY_WORK_YIELD;
        case WorkResult::YieldUntil:
            return ENTROPY_WORK_YIELD;  // C API doesn't support timed yields yet
        default:
            return ENTROPY_WORK_COMPLETE;
    }
}

// Convert C WorkResult to C++ WorkResultContext
WorkResultContext to_cpp_work_result_context(EntropyWorkResult result) {
    switch (result) {
        case ENTROPY_WORK_COMPLETE:
            return WorkResultContext::complete();
        case ENTROPY_WORK_YIELD:
            return WorkResultContext::yield();
        default:
            return WorkResultContext::complete();
    }
}

// Convert C++ WorkGraphConfig to C struct
EntropyWorkGraphConfig to_c_config(const WorkGraphConfig& config) {
    EntropyWorkGraphConfig c_config;
    c_config.enable_events = config.enableEvents ? ENTROPY_TRUE : ENTROPY_FALSE;
    c_config.enable_state_manager = config.enableStateManager ? ENTROPY_TRUE : ENTROPY_FALSE;
    c_config.enable_advanced_scheduling = config.enableAdvancedScheduling ? ENTROPY_TRUE : ENTROPY_FALSE;
    c_config.expected_node_count = config.expectedNodeCount;
    c_config.max_deferred_nodes = config.maxDeferredNodes;
    c_config.max_deferred_processing_iterations = config.maxDeferredProcessingIterations;
    c_config.enable_debug_logging = config.enableDebugLogging ? ENTROPY_TRUE : ENTROPY_FALSE;
    c_config.enable_debug_registration = config.enableDebugRegistration ? ENTROPY_TRUE : ENTROPY_FALSE;
    return c_config;
}

// Convert C config to C++ WorkGraphConfig
WorkGraphConfig to_cpp_config(const EntropyWorkGraphConfig* config) {
    WorkGraphConfig cpp_config;
    if (!config) return cpp_config;  // Return defaults

    cpp_config.enableEvents = config->enable_events != ENTROPY_FALSE;
    cpp_config.enableStateManager = config->enable_state_manager != ENTROPY_FALSE;
    cpp_config.enableAdvancedScheduling = config->enable_advanced_scheduling != ENTROPY_FALSE;
    cpp_config.expectedNodeCount = config->expected_node_count;
    cpp_config.maxDeferredNodes = config->max_deferred_nodes;
    cpp_config.maxDeferredProcessingIterations = config->max_deferred_processing_iterations;
    cpp_config.enableDebugLogging = config->enable_debug_logging != ENTROPY_FALSE;
    cpp_config.enableDebugRegistration = config->enable_debug_registration != ENTROPY_FALSE;
    return cpp_config;
}

}  // anonymous namespace

// ============================================================================
// Helper Functions Implementation
// ============================================================================

extern "C" {

void entropy_work_graph_config_init(EntropyWorkGraphConfig* config) {
    if (!config) return;

    config->enable_events = ENTROPY_FALSE;
    config->enable_state_manager = ENTROPY_FALSE;
    config->enable_advanced_scheduling = ENTROPY_FALSE;
    config->expected_node_count = 16;
    config->max_deferred_nodes = 0;  // Unlimited
    config->max_deferred_processing_iterations = 10;
    config->enable_debug_logging = ENTROPY_FALSE;
    config->enable_debug_registration = ENTROPY_FALSE;
}

const char* entropy_node_state_to_string(EntropyNodeState state) {
    switch (state) {
        case ENTROPY_NODE_PENDING:
            return "Pending";
        case ENTROPY_NODE_READY:
            return "Ready";
        case ENTROPY_NODE_SCHEDULED:
            return "Scheduled";
        case ENTROPY_NODE_EXECUTING:
            return "Executing";
        case ENTROPY_NODE_COMPLETED:
            return "Completed";
        case ENTROPY_NODE_FAILED:
            return "Failed";
        case ENTROPY_NODE_CANCELLED:
            return "Cancelled";
        case ENTROPY_NODE_YIELDED:
            return "Yielded";
        default:
            return "Unknown";
    }
}

const char* entropy_work_result_to_string(EntropyWorkResult result) {
    switch (result) {
        case ENTROPY_WORK_COMPLETE:
            return "Complete";
        case ENTROPY_WORK_YIELD:
            return "Yield";
        default:
            return "Unknown";
    }
}

// ============================================================================
// WorkGraph Lifecycle Implementation
// ============================================================================

entropy_WorkGraph entropy_work_graph_create(entropy_WorkContractGroup work_group, EntropyStatus* status) {
    if (!status) return nullptr;
    *status = ENTROPY_OK;

    if (!work_group) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        WorkContractGroup* cpp_group = reinterpret_cast<WorkContractGroup*>(work_group);
        auto* graph = new (std::nothrow) WorkGraph(cpp_group);
        if (!graph) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        return to_c(graph);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_WorkGraph entropy_work_graph_create_with_config(entropy_WorkContractGroup work_group,
                                                        const EntropyWorkGraphConfig* config, EntropyStatus* status) {
    if (!status) return nullptr;
    *status = ENTROPY_OK;

    if (!work_group || !config) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        WorkContractGroup* cpp_group = reinterpret_cast<WorkContractGroup*>(work_group);
        WorkGraphConfig cpp_config = to_cpp_config(config);

        auto* graph = new (std::nothrow) WorkGraph(cpp_group, cpp_config);
        if (!graph) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }
        return to_c(graph);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

void entropy_work_graph_destroy(entropy_WorkGraph graph) {
    if (!graph) return;

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        delete cpp_graph;
    } catch (...) {
        // Swallow exceptions during destruction
    }
}

// ============================================================================
// Node Creation Implementation
// ============================================================================

entropy_NodeHandle entropy_work_graph_add_node(entropy_WorkGraph graph, EntropyWorkCallback callback, void* user_data,
                                               const char* name, EntropyExecutionType execution_type,
                                               EntropyStatus* status) {
    if (!status) return nullptr;
    *status = ENTROPY_OK;

    if (!graph || !callback) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        WorkGraph* cpp_graph = to_cpp(graph);

        // Wrap the C callback in a std::function
        std::function<void()> work = [callback, user_data]() noexcept { callback(user_data); };

        std::string node_name = name ? name : "";
        ExecutionType cpp_exec_type = to_cpp_execution_type(execution_type);

        // Add the node
        WorkGraph::NodeHandle cpp_handle = cpp_graph->addNode(work, node_name, user_data, cpp_exec_type);

        // Allocate a C++ handle on the heap to return to C code
        auto* handle_ptr = new (std::nothrow) WorkGraph::NodeHandle(cpp_handle);
        if (!handle_ptr) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }

        return to_c_node(handle_ptr);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

entropy_NodeHandle entropy_work_graph_add_yieldable_node(entropy_WorkGraph graph, EntropyYieldableWorkCallback callback,
                                                         void* user_data, const char* name,
                                                         EntropyExecutionType execution_type, uint32_t max_reschedules,
                                                         EntropyStatus* status) {
    if (!status) return nullptr;
    *status = ENTROPY_OK;

    if (!graph || !callback) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return nullptr;
    }

    try {
        WorkGraph* cpp_graph = to_cpp(graph);

        // Wrap the C callback in a YieldableWorkFunction
        YieldableWorkFunction work = [callback, user_data]() -> WorkResultContext {
            EntropyWorkResult c_result = callback(user_data);
            return to_cpp_work_result_context(c_result);
        };

        std::string node_name = name ? name : "";
        ExecutionType cpp_exec_type = to_cpp_execution_type(execution_type);

        std::optional<uint32_t> max_reschedule_opt =
            (max_reschedules > 0) ? std::optional<uint32_t>(max_reschedules) : std::nullopt;

        // Add the yieldable node
        WorkGraph::NodeHandle cpp_handle =
            cpp_graph->addYieldableNode(work, node_name, user_data, cpp_exec_type, max_reschedule_opt);

        // Allocate a C++ handle on the heap
        auto* handle_ptr = new (std::nothrow) WorkGraph::NodeHandle(cpp_handle);
        if (!handle_ptr) {
            *status = ENTROPY_ERR_NO_MEMORY;
            return nullptr;
        }

        return to_c_node(handle_ptr);
    } catch (...) {
        translate_exception(status);
        return nullptr;
    }
}

// ============================================================================
// Dependency Management Implementation
// ============================================================================

void entropy_work_graph_add_dependency(entropy_WorkGraph graph, entropy_NodeHandle from, entropy_NodeHandle to,
                                       EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!graph || !from || !to) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        WorkGraph::NodeHandle* cpp_from = to_cpp_node(from);
        WorkGraph::NodeHandle* cpp_to = to_cpp_node(to);

        cpp_graph->addDependency(*cpp_from, *cpp_to);
    } catch (...) {
        translate_exception(status);
    }
}

// ============================================================================
// Execution Control Implementation
// ============================================================================

void entropy_work_graph_execute(entropy_WorkGraph graph, EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!graph) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        cpp_graph->execute();
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_work_graph_suspend(entropy_WorkGraph graph, EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!graph) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        cpp_graph->suspend();
    } catch (...) {
        translate_exception(status);
    }
}

void entropy_work_graph_resume(entropy_WorkGraph graph, EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!graph) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        cpp_graph->resume();
    } catch (...) {
        translate_exception(status);
    }
}

EntropyBool entropy_work_graph_is_suspended(entropy_WorkGraph graph) {
    if (!graph) return ENTROPY_FALSE;

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        return cpp_graph->isSuspended() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        return ENTROPY_FALSE;
    }
}

void entropy_work_graph_wait(entropy_WorkGraph graph, EntropyWaitResult* result, EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!graph || !result) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        WorkGraph::WaitResult cpp_result = cpp_graph->wait();

        // Convert to C result
        result->all_completed = cpp_result.allCompleted ? ENTROPY_TRUE : ENTROPY_FALSE;
        result->dropped_count = cpp_result.droppedCount;
        result->failed_count = cpp_result.failedCount;
        result->completed_count = cpp_result.completedCount;
    } catch (...) {
        translate_exception(status);
    }
}

EntropyBool entropy_work_graph_is_complete(entropy_WorkGraph graph) {
    if (!graph) return ENTROPY_FALSE;

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        return cpp_graph->isComplete() ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        return ENTROPY_FALSE;
    }
}

// ============================================================================
// Statistics and Monitoring Implementation
// ============================================================================

void entropy_work_graph_get_stats(entropy_WorkGraph graph, EntropyWorkGraphStats* stats, EntropyStatus* status) {
    if (!status) return;
    *status = ENTROPY_OK;

    if (!graph || !stats) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return;
    }

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        WorkGraphStats::Snapshot cpp_stats = cpp_graph->getStats();

        // Convert to C stats
        stats->total_nodes = cpp_stats.totalNodes;
        stats->completed_nodes = cpp_stats.completedNodes;
        stats->failed_nodes = cpp_stats.failedNodes;
        stats->cancelled_nodes = cpp_stats.cancelledNodes;
        stats->pending_nodes = cpp_stats.pendingNodes;
        stats->ready_nodes = cpp_stats.readyNodes;
        stats->scheduled_nodes = cpp_stats.scheduledNodes;
        stats->executing_nodes = cpp_stats.executingNodes;
        stats->memory_usage = cpp_stats.memoryUsage;
    } catch (...) {
        translate_exception(status);
    }
}

uint32_t entropy_work_graph_get_pending_count(entropy_WorkGraph graph) {
    if (!graph) return 0;

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        return cpp_graph->getPendingCount();
    } catch (...) {
        return 0;
    }
}

// ============================================================================
// Node Handle Operations Implementation
// ============================================================================

EntropyBool entropy_node_handle_is_valid(entropy_WorkGraph graph, entropy_NodeHandle handle) {
    if (!graph || !handle) return ENTROPY_FALSE;

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        WorkGraph::NodeHandle* cpp_handle = to_cpp_node(handle);
        return cpp_graph->isHandleValid(*cpp_handle) ? ENTROPY_TRUE : ENTROPY_FALSE;
    } catch (...) {
        return ENTROPY_FALSE;
    }
}

EntropyNodeState entropy_node_handle_get_state(entropy_WorkGraph graph, entropy_NodeHandle handle,
                                               EntropyStatus* status) {
    if (!status) return ENTROPY_NODE_PENDING;
    *status = ENTROPY_OK;

    if (!graph || !handle) {
        *status = ENTROPY_ERR_INVALID_ARG;
        return ENTROPY_NODE_PENDING;
    }

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        WorkGraph::NodeHandle* cpp_handle = to_cpp_node(handle);

        const WorkGraphNode* node = cpp_graph->getNodeData(*cpp_handle);
        if (!node) {
            *status = ENTROPY_ERR_NOT_FOUND;
            return ENTROPY_NODE_PENDING;
        }

        return to_c_node_state(node->state.load());
    } catch (...) {
        translate_exception(status);
        return ENTROPY_NODE_PENDING;
    }
}

const char* entropy_node_handle_get_name(entropy_WorkGraph graph, entropy_NodeHandle handle) {
    if (!graph || !handle) return nullptr;

    try {
        WorkGraph* cpp_graph = to_cpp(graph);
        WorkGraph::NodeHandle* cpp_handle = to_cpp_node(handle);

        const WorkGraphNode* node = cpp_graph->getNodeData(*cpp_handle);
        if (!node) return nullptr;

        return node->name.c_str();
    } catch (...) {
        return nullptr;
    }
}

void entropy_node_handle_destroy(entropy_NodeHandle handle) {
    if (!handle) return;

    try {
        WorkGraph::NodeHandle* cpp_handle = to_cpp_node(handle);
        delete cpp_handle;
    } catch (...) {
        // Swallow exceptions during destruction
    }
}

}  // extern "C"
