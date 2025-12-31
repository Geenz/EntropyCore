/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file LogLevel.h
 * @brief Log severity levels for controlling output verbosity
 *
 * This file defines severity levels from Trace to Fatal that control log verbosity.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace EntropyEngine
{
namespace Core
{
namespace Logging
{

/**
 * @brief Log severity levels for controlling output verbosity
 *
 * Ordered from least to most severe. Setting a minimum level includes
 * all more severe levels. E.g., Warning includes Warning, Error, Fatal.
 */
enum class LogLevel : uint8_t
{
    Trace = 0,    ///< Detailed trace information for deep debugging
    Debug = 1,    ///< Debug information useful during development
    Info = 2,     ///< Informational messages about normal operation
    Warning = 3,  ///< Warning conditions that might require attention
    Error = 4,    ///< Error conditions that require immediate attention
    Fatal = 5,    ///< Fatal errors that will terminate the application
    Off = 6       ///< Disable all logging output
};

/**
 * @brief Convert log level to string representation
 *
 * Returns fixed-width strings (5 chars) for consistent alignment.
 *
 * @param level The log level to convert
 * @return Level name ("TRACE", "DEBUG", etc.)
 *
 * @code
 * std::cout << "[" << logLevelToString(LogLevel::Error) << "] "
 *           << "Failed to open file" << std::endl;
 * // Output: "[ERROR] Failed to open file"
 * @endcode
 */
inline constexpr std::string_view logLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Warning:
            return "WARN ";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
        case LogLevel::Off:
            return "OFF  ";
    }
    return "UNKNOWN";
}

/**
 * @brief Convert log level to single character
 *
 * Useful for compact formats and grep operations.
 *
 * @param level The log level to convert
 * @return Single character ('T', 'D', 'I', 'W', 'E', 'F', 'O')
 *
 * @code
 * // Compact format for high-volume logging
 * std::cout << "[" << logLevelToChar(level) << "] "
 *           << timestamp << " " << message << std::endl;
 * // Output: "[E] 2024-01-15 10:30:45 Connection lost"
 * @endcode
 */
inline constexpr char logLevelToChar(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return 'T';
        case LogLevel::Debug:
            return 'D';
        case LogLevel::Info:
            return 'I';
        case LogLevel::Warning:
            return 'W';
        case LogLevel::Error:
            return 'E';
        case LogLevel::Fatal:
            return 'F';
        case LogLevel::Off:
            return 'O';
    }
    return '?';
}

/**
 * @brief Convert string to LogLevel
 *
 * Case-insensitive. Accepts "WARN" for "WARNING". Returns Info as
 * default for unrecognized strings.
 *
 * @param str The string to parse
 * @return Corresponding LogLevel, or Info if parsing fails
 *
 * @code
 * // Reading from configuration
 * auto level = stringToLogLevel(config["log_level"]);
 * logger.setLevel(level);
 *
 * // Accepted formats:
 * stringToLogLevel("debug");   // LogLevel::Debug
 * stringToLogLevel("DEBUG");   // LogLevel::Debug
 * stringToLogLevel("WARN");    // LogLevel::Warning
 * stringToLogLevel("invalid"); // LogLevel::Info (default)
 * @endcode
 */
inline LogLevel stringToLogLevel(const std::string& str) {
    if (str == "Trace" || str == "TRACE") return LogLevel::Trace;
    if (str == "Debug" || str == "DEBUG") return LogLevel::Debug;
    if (str == "Info" || str == "INFO") return LogLevel::Info;
    if (str == "Warning" || str == "WARNING" || str == "WARN") return LogLevel::Warning;
    if (str == "Error" || str == "ERROR") return LogLevel::Error;
    if (str == "Fatal" || str == "FATAL") return LogLevel::Fatal;
    if (str == "Off" || str == "OFF") return LogLevel::Off;
    return LogLevel::Info;  // Default
}

}  // namespace Logging
}  // namespace Core
}  // namespace EntropyEngine
