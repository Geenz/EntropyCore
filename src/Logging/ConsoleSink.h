/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file ConsoleSink.h
 * @brief Console log sink with color support
 *
 * This file implements a console sink that outputs logs to stdout/stderr with
 * optional ANSI color codes for improved readability and visual distinction
 * between log levels.
 */

#pragma once

#include <iostream>
#include <mutex>

#include "ILogSink.h"

namespace EntropyEngine
{
namespace Core
{
namespace Logging
{

/**
 * @brief Log sink that outputs to console
 *
 * ConsoleSink provides formatted output to the terminal with optional color
 * coding based on log severity. Error and Fatal messages are directed to
 * stderr while other levels use stdout, allowing flexible output redirection.
 *
 * Features:
 * - Color-coded output by log level for visual distinction
 * - Thread-safe to prevent garbled output from concurrent logging
 * - Configurable format options (thread IDs, source locations)
 * - Intelligent stream selection (stderr for errors, stdout for others)
 *
 * Usage tip: Redirect stdout while preserving error visibility:
 *   ./myapp > output.log  # Errors remain visible on console
 */
class ConsoleSink : public ILogSink
{
private:
    mutable std::mutex _mutex;
    LogLevel _minLevel = LogLevel::Trace;
    bool _useColor = true;
    bool _showThreadId = true;
    bool _showLocation = false;

    /// ANSI color codes for different log levels
    static constexpr const char* RESET = "\033[0m";
    static constexpr const char* RED = "\033[31m";
    static constexpr const char* YELLOW = "\033[33m";
    static constexpr const char* GREEN = "\033[32m";
    static constexpr const char* CYAN = "\033[36m";
    static constexpr const char* MAGENTA = "\033[35m";
    static constexpr const char* GRAY = "\033[90m";

public:
    explicit ConsoleSink(bool useColor = true, bool showThreadId = true)
        : _useColor(useColor), _showThreadId(showThreadId) {}

    void write(const LogEntry& entry) override;
    void flush() override;
    bool shouldLog(LogLevel level) const override;
    void setMinLevel(LogLevel level) override;

    /**
     * @brief Enable/disable color output
     *
     * Disable when terminal doesn't support ANSI or piping to file.
     *
     * @param useColor true to enable colors, false for plain text
     */
    void setUseColor(bool useColor) {
        _useColor = useColor;
    }

    /**
     * @brief Enable/disable thread ID in output
     *
     * Useful for multithreaded debugging but noisy in single-threaded apps.
     *
     * @param show true to include thread IDs, false to hide them
     */
    void setShowThreadId(bool show) {
        _showThreadId = show;
    }

    /**
     * @brief Enable/disable source location in output
     *
     * Shows file:line info. Helpful for debugging but verbose for production.
     *
     * @param show true to include file:line info, false to hide it
     */
    void setShowLocation(bool show) {
        _showLocation = show;
    }

private:
    /// Get ANSI color code for a log level
    const char* getColorForLevel(LogLevel level) const;
    /// Format and write a log entry to the appropriate stream
    void formatAndWrite(std::ostream& stream, const LogEntry& entry);
};

}  // namespace Logging
}  // namespace Core
}  // namespace EntropyEngine
