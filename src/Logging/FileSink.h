/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file FileSink.h
 * @brief File log sink for persistent log output
 *
 * This file implements a sink that writes logs to a file on disk. Output is
 * plain text with no ANSI escapes, and every entry carries a full date so the
 * file remains readable long after the process that wrote it has exited.
 */

#pragma once

#include <fstream>
#include <mutex>
#include <string>

#include "ILogSink.h"

namespace EntropyEngine
{
namespace Core
{
namespace Logging
{

/**
 * @brief Log sink that writes to a file
 *
 * FileSink is the persistent counterpart to ConsoleSink: same entry format,
 * same threading guarantees, but written to disk and never colored. It is
 * typically registered alongside a console sink with a lower minimum level, so
 * the terminal stays readable while the file keeps the full trace.
 *
 * Features:
 * - Plain text output - no ANSI codes, which would corrupt a log file
 * - Full date in every timestamp, unlike the console's time-only format
 * - Thread-safe; concurrent writers cannot interleave within a line
 * - Append or truncate on open, selected at construction
 *
 * Failure policy: if the file cannot be opened the sink stays constructed but
 * inert - write() becomes a no-op rather than throwing. A logging sink that
 * takes down the process because a path was bad is worse than one that is
 * quietly absent, and isOpen() is available for callers that need to know.
 *
 * @code
 * auto file = std::make_shared<FileSink>("entropy.log");
 * if (!file->isOpen()) {  // optional - the sink is safe to use either way
 *     std::cerr << "log file unavailable, continuing without it\n";
 * }
 * file->setMinLevel(LogLevel::Trace);   // capture everything on disk
 * logger.addSink(file);
 * @endcode
 */
class FileSink : public ILogSink
{
private:
    mutable std::mutex _mutex;
    std::ofstream _file;
    LogLevel _minLevel = LogLevel::Trace;
    bool _showThreadId = true;
    bool _showLocation = true;

public:
    /**
     * @brief Open a log file for writing
     *
     * @param path     Filesystem path to write to
     * @param append   true to append to an existing file, false to truncate
     * @param showThreadId true to include thread IDs in each entry
     */
    explicit FileSink(const std::string& path, bool append = true, bool showThreadId = true);

    /// Flushes and closes the file. Buffered entries are not lost on shutdown.
    ~FileSink() override;

    void write(const LogEntry& entry) override;
    void flush() override;
    bool shouldLog(LogLevel level) const override;
    void setMinLevel(LogLevel level) override;

    /**
     * @brief Check whether the file was opened successfully
     *
     * A sink that failed to open is inert, not broken - write() simply does
     * nothing. Query this when the caller wants to report or fall back.
     *
     * @return true if the file is open and writable
     */
    bool isOpen() const;

    /**
     * @brief Enable/disable thread ID in output
     *
     * @param show true to include thread IDs, false to hide them
     */
    void setShowThreadId(bool show) {
        _showThreadId = show;
    }

    /**
     * @brief Enable/disable source location in output
     *
     * Defaults to true here, unlike ConsoleSink: file logs are read after the
     * fact, when file:line is usually the whole point.
     *
     * @param show true to include file:line info, false to hide it
     */
    void setShowLocation(bool show) {
        _showLocation = show;
    }

private:
    /// Format and write a log entry to the file. Caller must hold _mutex.
    void formatAndWrite(const LogEntry& entry);
};

}  // namespace Logging
}  // namespace Core
}  // namespace EntropyEngine
