/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "FileSink.h"

#include <iomanip>
#include <sstream>

namespace EntropyEngine
{
namespace Core
{
namespace Logging
{

FileSink::FileSink(const std::string& path, bool append, bool showThreadId)
    : _path(path), _showThreadId(showThreadId) {
    const auto mode = append ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc);
    _file.open(path, mode);
    // Deliberately no throw and no diagnostic here: a sink that cannot open its
    // file is inert (see write()), and reporting the failure through the logger
    // we are in the middle of constructing would be circular.
}

FileSink::~FileSink() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_file.is_open()) {
        _file.flush();
        _file.close();
    }
}

void FileSink::write(const LogEntry& entry) {
    if (!shouldLog(entry.level)) return;

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_file.is_open()) return;  // inert sink - see the constructor
    formatAndWrite(entry);
}

void FileSink::flush() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_file.is_open()) _file.flush();
}

bool FileSink::shouldLog(LogLevel level) const {
    return level >= _minLevel;
}

void FileSink::setMinLevel(LogLevel level) {
    _minLevel = level;
}

bool FileSink::isOpen() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _file.is_open();
}

void FileSink::formatAndWrite(const LogEntry& entry) {
    // Format: [YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [THREAD?] [CATEGORY] MESSAGE [LOCATION?]
    // Matches ConsoleSink field-for-field except for the date and the absence of
    // colour, so the two are diffable when both sinks are attached.

    auto timeVal = std::chrono::system_clock::to_time_t(entry.timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(entry.timestamp.time_since_epoch()) % 1000;

    // localtime is not thread-safe; both branches use the reentrant form. We
    // already hold _mutex, but that only serialises THIS sink - the console sink
    // can be formatting concurrently on another thread.
#ifdef _WIN32
    std::tm tmBuf;
    localtime_s(&tmBuf, &timeVal);
#else
    std::tm tmBuf;
    localtime_r(&timeVal, &tmBuf);
#endif
    _file << "[" << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
    _file << "." << std::setfill('0') << std::setw(3) << ms.count() << "] ";

    // Level - no colour: ANSI escapes in a log file corrupt every downstream
    // grep, tail and parser that reads it.
    _file << "[" << logLevelToString(entry.level) << "] ";

    // Thread ID (optional)
    if (_showThreadId) {
        std::ostringstream threadStr;
        threadStr << entry.threadId;
        auto threadIdStr = threadStr.str();

        // Truncate to the last 4 characters, same as ConsoleSink, so entries
        // from the two sinks line up.
        if (threadIdStr.length() > 4) {
            threadIdStr = threadIdStr.substr(threadIdStr.length() - 4);
        }
        _file << "[" << std::setw(4) << threadIdStr << "] ";
    }

    // Category
    if (!entry.category.empty()) {
        _file << "[" << entry.category << "] ";
    }

    // Message
    _file << entry.message;

    // Source location (optional, on by default for files)
    if (_showLocation && entry.location.line() != 0) {
        _file << " (" << entry.location.file_name() << ":" << entry.location.line() << ")";
    }

    _file << '\n';

    // Flush at Error and above so a crash cannot swallow the entries that
    // explain it. Lower levels stay buffered - flushing every Trace line would
    // dominate the cost of logging.
    if (entry.level >= LogLevel::Error) _file.flush();
}

}  // namespace Logging
}  // namespace Core
}  // namespace EntropyEngine
