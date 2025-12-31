/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file LogEntry.h
 * @brief Log entry structure for comprehensive log information
 *
 * This file defines LogEntry, the fundamental data structure that carries all
 * information about a single log event, including timestamp, severity, source
 * location, and contextual details.
 */

#pragma once

#include <chrono>
#include <source_location>
#include <string>
#include <thread>

#include "LogLevel.h"

namespace EntropyEngine
{
namespace Core
{
namespace Logging
{

/**
 * @brief A single log entry containing all metadata and message
 *
 * LogEntry captures comprehensive information about each log event including
 * the message content, timestamp, source location, thread ID, severity level,
 * and category. This provides complete context for debugging and analysis.
 *
 * Design principles:
 * - Complete: Contains all necessary debugging context
 * - Flexible: Sinks determine final formatting and presentation
 * - Thread-safe: Safe to pass between threads
 *
 * Formatting is deferred to sinks. The entry captures raw data,
 * allowing background threads to handle formatting operations.
 */
struct LogEntry
{
    /// Timestamp when the log was created
    std::chrono::system_clock::time_point timestamp;

    /// Thread ID that created the log
    std::thread::id threadId;

    /// Log severity level
    LogLevel level;

    /// Category/module name (e.g., "WorkGraph", "Renderer")
    std::string_view category;

    /// The actual log message
    std::string message;

    /// Source location information (file:line)
    std::source_location location;

    /// Optional: Thread-local context (e.g., current work item)
    void* context = nullptr;

    /**
     * @brief Construct a log entry with current timestamp and thread ID
     *
     * Automatically captures time, thread ID, and source location using
     * C++20 source_location.
     *
     * @param lvl Log severity level
     * @param cat Category/subsystem name
     * @param msg Log message content
     * @param loc Source location (captured automatically)
     *
     * @code
     * // Location is captured automatically at the call site
     * LogEntry entry(LogLevel::Error, "Database", "Connection failed");
     * // entry.location contains the file:line information
     * @endcode
     */
    LogEntry(LogLevel lvl, std::string_view cat, std::string msg,
             const std::source_location& loc = std::source_location::current())
        : timestamp(std::chrono::system_clock::now()),
          threadId(std::this_thread::get_id()),
          level(lvl),
          category(cat),
          message(std::move(msg)),
          location(loc) {}
};

}  // namespace Logging
}  // namespace Core
}  // namespace EntropyEngine
