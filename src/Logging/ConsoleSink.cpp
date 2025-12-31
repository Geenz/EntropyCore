/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "ConsoleSink.h"

#include <iomanip>
#include <sstream>

namespace EntropyEngine
{
namespace Core
{
namespace Logging
{

void ConsoleSink::write(const LogEntry& entry) {
    if (!shouldLog(entry.level)) return;

    std::lock_guard<std::mutex> lock(_mutex);

    // Error and Fatal go to stderr, others to stdout
    auto& stream = (entry.level >= LogLevel::Error) ? std::cerr : std::cout;
    formatAndWrite(stream, entry);
}

void ConsoleSink::flush() {
    std::lock_guard<std::mutex> lock(_mutex);
    std::cout.flush();
    std::cerr.flush();
}

bool ConsoleSink::shouldLog(LogLevel level) const {
    return level >= _minLevel;
}

void ConsoleSink::setMinLevel(LogLevel level) {
    _minLevel = level;
}

const char* ConsoleSink::getColorForLevel(LogLevel level) const {
    if (!_useColor) return "";

    switch (level) {
        case LogLevel::Trace:
            return GRAY;
        case LogLevel::Debug:
            return CYAN;
        case LogLevel::Info:
            return GREEN;
        case LogLevel::Warning:
            return YELLOW;
        case LogLevel::Error:
            return RED;
        case LogLevel::Fatal:
            return MAGENTA;
        default:
            return RESET;
    }
}

void ConsoleSink::formatAndWrite(std::ostream& stream, const LogEntry& entry) {
    // Format: [TIMESTAMP] [LEVEL] [THREAD?] [CATEGORY] MESSAGE [LOCATION?]

    // Timestamp
    auto time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(entry.timestamp.time_since_epoch()) % 1000;

#ifdef _WIN32
    std::tm tm_buf;
    localtime_s(&tm_buf, &time_t);
    stream << "[" << std::put_time(&tm_buf, "%H:%M:%S");
#else
    std::tm tm_buf;
    localtime_r(&time_t, &tm_buf);
    stream << "[" << std::put_time(&tm_buf, "%H:%M:%S");
#endif
    stream << "." << std::setfill('0') << std::setw(3) << ms.count() << "] ";

    // Level with color
    if (_useColor) stream << getColorForLevel(entry.level);
    stream << "[" << logLevelToString(entry.level) << "]";
    if (_useColor) stream << RESET;
    stream << " ";

    // Thread ID (optional)
    if (_showThreadId) {
        std::ostringstream threadStr;
        threadStr << entry.threadId;
        auto threadIdStr = threadStr.str();

        // Truncate thread ID to last 4 characters for readability
        if (threadIdStr.length() > 4) {
            threadIdStr = threadIdStr.substr(threadIdStr.length() - 4);
        }
        stream << "[" << std::setw(4) << threadIdStr << "] ";
    }

    // Category
    if (!entry.category.empty()) {
        stream << "[" << entry.category << "] ";
    }

    // Message
    stream << entry.message;

    // Source location (optional)
    if (_showLocation && entry.location.line() != 0) {
        stream << " (" << entry.location.file_name() << ":" << entry.location.line() << ")";
    }

    stream << std::endl;
}

}  // namespace Logging
}  // namespace Core
}  // namespace EntropyEngine
