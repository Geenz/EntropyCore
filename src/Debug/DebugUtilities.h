/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file DebugUtilities.h
 * @brief Platform-specific debug helpers and assertion macros
 * 
 * This file provides cross-platform debugging utilities including debugger detection,
 * breakpoints, assertions, timing utilities, and memory tracking.
 */

#pragma once

#include <string_view>
#include <string>
#include <source_location>
#include <format>
#include <chrono>
#include <fstream>
#include "../Logging/Logger.h"

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace EntropyEngine {
namespace Core {
namespace Debug {

    /**
     * @brief Trigger a debugger breakpoint
     * 
     * Stops execution when a debugger is attached. Uses platform-specific
     * intrinsics for clean breakpoint handling.
     * 
     * @code
     * if (criticalErrorOccurred) {
     *     LOG_ERROR("Critical error detected");
     *     debugBreak();  // Stop execution for debugging
     * }
     * @endcode
     */
    inline void debugBreak() {
#if defined(_MSC_VER)
        __debugbreak();
#elif defined(__clang__) || defined(__GNUC__)
        __builtin_trap();
#else
        // Fallback - cause a segfault
        *static_cast<volatile int*>(nullptr) = 0;
#endif
    }

    /**
     * @brief Check if a debugger is attached to the process
     * 
     * Useful for conditional behavior like enhanced validation during debugging.
     * Works on Windows (IsDebuggerPresent), macOS (sysctl), and Linux (/proc).
     * 
     * @return true if a debugger is attached, false otherwise
     * 
     * @code
     * if (isDebuggerAttached()) {
     *     // Enable additional validation when debugging
     *     validateAllInvariants();
     *     dumpDetailedState();
     * }
     * @endcode
     */
    inline bool isDebuggerAttached() {
#if defined(_WIN32)
        return IsDebuggerPresent() != 0;
#elif defined(__APPLE__)
        // Check for debugger on macOS
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
        struct kinfo_proc info;
        size_t size = sizeof(info);
        info.kp_proc.p_flag = 0;
        sysctl(mib, 4, &info, &size, nullptr, 0);
        return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
        // Linux - check TracerPid in /proc/self/status
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.compare(0, 10, "TracerPid:") == 0) {
                return std::stoi(line.substr(10)) != 0;
            }
        }
        return false;
#endif
    }

    /**
     * @brief Debug assertion that only runs in debug builds
     * 
     * Validates conditions during development. Logs fatal error and breaks on
     * failure. Compiles to nothing in release builds.
     * 
     * Usage:
     * ENTROPY_DEBUG_ASSERT(pointer != nullptr, "Widget pointer must not be null");
     * ENTROPY_DEBUG_ASSERT(count > 0, "Requires at least one item to process");
     */
    #ifdef EntropyDebug
        #define ENTROPY_DEBUG_ASSERT(condition, message) \
            do { \
                if (!(condition)) { \
                    auto msg = ::EntropyEngine::Core::Debug::debugFormat( \
                        "Assertion failed: {} - {}", \
                        #condition, \
                        message \
                    ); \
                    ::EntropyEngine::Core::Logging::Logger::global().fatal( \
                        "Assertion", \
                        msg \
                    ); \
                    ::EntropyEngine::Core::Debug::debugBreak(); \
                } \
            } while(0)
    #else
        #define ENTROPY_DEBUG_ASSERT(condition, message) ((void)0)
    #endif

    /**
     * @brief Debug-only code block
     * 
     * Excludes wrapped code from release builds. Useful for expensive validation
     * or debug output that would impact production performance.
     * 
     * @code
     * ENTROPY_DEBUG_ONLY({
     *     // This block is excluded from release builds
     *     validateDataStructure();
     *     dumpStateToFile("debug_state.txt");
     *     checkMemoryLeaks();
     * });
     * @endcode
     */
    #ifdef EntropyDebug
        #define ENTROPY_DEBUG_ONLY(code) do { code } while(0)
    #else
        #define ENTROPY_DEBUG_ONLY(code) ((void)0)
    #endif

    /**
     * @brief Mark a variable as used only in debug builds
     * 
     * Prevents unused variable warnings in release builds by marking as
     * [[maybe_unused]] when debug code is disabled.
     * 
     * @code
     * void processData(const Data& data) {
     *     ENTROPY_DEBUG_VARIABLE(size_t originalSize) = data.size();
     *     
     *     // ... process data ...
     *     
     *     ENTROPY_DEBUG_ASSERT(data.size() >= originalSize, 
     *                          "Data size must not decrease during processing");
     * }
     * @endcode
     */
    #ifdef EntropyDebug
        #define ENTROPY_DEBUG_VARIABLE(x) x
    #else
        #define ENTROPY_DEBUG_VARIABLE(x) [[maybe_unused]] x
    #endif

    /**
     * @brief Helper to create formatted debug strings
     * 
     * Type-safe string formatting using C++20's std::format.
     * 
     * @tparam Args Variadic template arguments
     * @param fmt Format string using std::format syntax
     * @param args Arguments to format
     * @return Formatted string
     * 
     * @code
     * auto msg = debugFormat("Object {} at position ({}, {}) has {} children",
     *                        obj.getName(), obj.x, obj.y, obj.getChildCount());
     * LOG_DEBUG(msg);
     * @endcode
     */
    template<typename... Args>
    [[nodiscard]] std::string debugFormat(std::format_string<Args...> fmt, Args&&... args) {
        return std::format(fmt, std::forward<Args>(args)...);
    }

    /**
     * @brief Scoped debug timer for measuring execution time
     * 
     * Automatically measures and logs execution time when destroyed. Uses
     * high-resolution clocks for microsecond precision.
     * 
     * @code
     * void expensiveOperation() {
     *     ScopedTimer timer("ExpensiveOperation");
     *     
     *     // Perform work
     *     processLargeDataset();
     *     
     * }  // Automatically logs: "ExpensiveOperation took 1234.567ms"
     * 
     * // Manual duration checking
     * {
     *     ScopedTimer timer("CustomTiming", false);  // Disable automatic logging
     *     doWork();
     *     if (timer.getDuration() > 100.0) {
     *         LOG_WARN("Operation exceeded time limit: {:.2f}ms", timer.getDuration());
     *     }
     * }
     * @endcode
     */
    class ScopedTimer {
    private:
        std::string_view _name;
        std::chrono::high_resolution_clock::time_point _start;
        bool _logOnDestruct;
        
    public:
        explicit ScopedTimer(std::string_view name, bool logOnDestruct = true)
            : _name(name)
            , _start(std::chrono::high_resolution_clock::now())
            , _logOnDestruct(logOnDestruct) {}
            
        ~ScopedTimer() {
            if (_logOnDestruct) {
                auto duration = getDuration();
                auto msg = debugFormat("{} took {:.3f}ms", _name, duration);
                ENTROPY_LOG_DEBUG_CAT("Timer", msg);
            }
        }
        
        [[nodiscard]] double getDuration() const {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration<double, std::milli>(end - _start);
            return duration.count();
        }
    };

    /**
     * @brief RAII helper for debug scope tracking
     * 
     * Logs entry and exit from code scopes. Provides execution flow visibility
     * in debug logs. RAII ensures exit logging even when exceptions are thrown.
     * 
     * @code
     * void complexAlgorithm() {
     *     DebugScope scope("ComplexAlgorithm");
     *     // Logs: "Entering: ComplexAlgorithm"
     *     
     *     if (someCondition) {
     *         DebugScope innerScope("ComplexAlgorithm::OptimizedPath");
     *         // Logs: "Entering: ComplexAlgorithm::OptimizedPath"
     *         doOptimizedWork();
     *         // Logs: "Leaving: ComplexAlgorithm::OptimizedPath"
     *     }
     *     
     *     // Logs: "Leaving: ComplexAlgorithm"
     * }
     * @endcode
     */
    class DebugScope {
    private:
        std::string_view _name;
        
    public:
        explicit DebugScope(std::string_view name) : _name(name) {
            auto msg = debugFormat("Entering: {}", _name);
            ENTROPY_LOG_TRACE_CAT("Scope", msg);
        }
        
        ~DebugScope() {
            auto msg = debugFormat("Leaving: {}", _name);
            ENTROPY_LOG_TRACE_CAT("Scope", msg);
        }
    };

    /**
     * @brief Macro for creating a debug scope
     * 
     * Simplified scope tracking. Generates unique variable names to allow
     * multiple uses. Compiles to nothing in release builds.
     * 
     * @code
     * void processRequest(const Request& req) {
     *     ENTROPY_DEBUG_SCOPE("ProcessRequest");
     *     
     *     validateRequest(req);
     *     
     *     {
     *         ENTROPY_DEBUG_SCOPE("ProcessRequest::DatabaseUpdate");
     *         updateDatabase(req);
     *     }
     *     
     *     sendResponse(req);
     * }
     * @endcode
     */
    #ifdef EntropyDebug
        #define ENTROPY_DEBUG_SCOPE(name) \
            ::EntropyEngine::Core::Debug::DebugScope _debugScope##__LINE__(name)
    #else
        #define ENTROPY_DEBUG_SCOPE(name) ((void)0)
    #endif

    /**
     * @brief Validate a pointer and log if null
     * 
     * Validates pointers and logs error with source location when null.
     * Uses C++20 source_location for automatic location capture.
     * 
     * @tparam T The pointed-to type
     * @param ptr The pointer to validate
     * @param name Human-readable name for the pointer
     * @param loc Source location (captured automatically)
     * @return true if pointer is valid, false if null
     * 
     * @code
     * void processWidget(Widget* widget) {
     *     if (!validatePointer(widget, "widget")) {
     *         return;  // Error logged with file:line information
     *     }
     *     
     *     // Pointer is valid
     *     widget->update();
     * }
     * @endcode
     */
    template<typename T>
    [[nodiscard]] bool validatePointer(T* ptr, std::string_view name, 
                                       const std::source_location& loc = std::source_location::current()) {
        if (!ptr) {
            auto msg = debugFormat("Null pointer: {} at {}:{}", 
                                  name, loc.file_name(), loc.line());
            ENTROPY_LOG_WARNING_CAT("Validation", msg);
            return false;
        }
        return true;
    }

    /**
     * @brief Memory usage tracking structure
     * 
     * Tracks allocations, deallocations, and peak usage. Would integrate with
     * memory allocator for real-time statistics in full implementation.
     * 
     * @code
     * auto stats = getMemoryStats();
     * LOG_INFO("Current memory: {} MB, Peak: {} MB",
     *          stats.currentBytes / (1024.0 * 1024.0),
     *          stats.peakBytes / (1024.0 * 1024.0));
     * 
     * if (stats.allocationCount > stats.deallocationCount + 1000) {
     *     LOG_WARN("Potential memory leak detected");
     * }
     * @endcode
     */
    struct MemoryStats {
        size_t currentBytes = 0;
        size_t peakBytes = 0;
        size_t allocationCount = 0;
        size_t deallocationCount = 0;
    };

    /**
     * @brief Get current memory statistics
     * 
     * Returns memory usage statistics. Stub implementation - integrate with
     * memory allocator for actual tracking. Debug builds only.
     * 
     * @return Current memory statistics
     */
    [[nodiscard]] inline MemoryStats getMemoryStats() {
        // This would hook into the memory system
        return MemoryStats{};
    }

} // namespace Debug
} // namespace Core
} // namespace EntropyEngine

