/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file ILogSink.h
 * @brief Log sink interface for output destinations
 *
 * This file defines the interface for log sinks - the destinations where log
 * messages are sent. Sinks can output to console, files, network services,
 * databases, or any other destination.
 */

#pragma once

#include <memory>

#include "LogEntry.h"

namespace EntropyEngine
{
namespace Core
{
namespace Logging
{

/**
 * @brief Interface for log sinks that handle log output
 *
 * Log sinks receive LogEntry objects and output them to their designated
 * destination. Each sink can implement its own formatting, filtering,
 * and delivery mechanisms.
 *
 * Common sink implementations:
 * - Console: Output to stdout/stderr with optional color
 * - File: Persistent storage with rotation support
 * - Network: Remote logging servers
 * - Database: Structured storage for analysis
 * - Memory: In-memory ring buffer
 *
 * Multiple sinks can operate independently with different configurations,
 * allowing flexible log routing based on severity or category.
 */
class ILogSink
{
public:
    virtual ~ILogSink() = default;

    /**
     * @brief Write a log entry to this sink
     *
     * Processes and outputs a log entry according to the sink's implementation.
     * Must be thread-safe. May buffer output for performance.
     *
     * @param entry The log entry to write
     *
     * @code
     * void MyCustomSink::write(const LogEntry& entry) {
     *     if (!shouldLog(entry.level)) return;
     *
     *     auto formatted = formatEntry(entry);
     *     sendToDestination(formatted);
     * }
     * @endcode
     */
    virtual void write(const LogEntry& entry) = 0;

    /**
     * @brief Flush any buffered data
     *
     * Forces immediate write of buffered data. Called after critical messages
     * and during shutdown. Sinks without buffering can implement as no-op.
     */
    virtual void flush() = 0;

    /**
     * @brief Check if this sink accepts logs at the given level
     *
     * Allows different sinks to filter messages independently based on
     * their configuration.
     *
     * @param level The log level to check
     * @return true if the sink will process logs at this level
     *
     * @code
     * // Console shows only warnings and above
     * consoleSink->setMinLevel(LogLevel::Warning);
     *
     * // File captures all messages including trace
     * fileSink->setMinLevel(LogLevel::Trace);
     * @endcode
     */
    virtual bool shouldLog(LogLevel level) const = 0;

    /**
     * @brief Set the minimum log level for this sink
     *
     * Controls verbosity of this specific sink. Each sink maintains its
     * own level setting for flexible log routing.
     *
     * @param level The minimum level to accept (inclusive)
     *
     * @code
     * // Development configuration
     * sink->setMinLevel(LogLevel::Debug);
     *
     * // Production configuration
     * sink->setMinLevel(LogLevel::Warning);
     *
     * // Debugging mode
     * sink->setMinLevel(LogLevel::Trace);
     * @endcode
     */
    virtual void setMinLevel(LogLevel level) = 0;
};

/// Convenience typedef for shared sink pointers
using LogSinkPtr = std::shared_ptr<ILogSink>;

}  // namespace Logging
}  // namespace Core
}  // namespace EntropyEngine
