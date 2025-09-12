/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file Logger.h
 * @brief Main logger class for centralized logging management
 * 
 * This file contains the Logger class, which serves as the central component of
 * the logging system. It manages multiple sinks, provides thread-safe logging,
 * and offers both a direct API and convenient macros for application-wide logging.
 */

#pragma once

#include "LogEntry.h"
#include "ILogSink.h"
#include <vector>
#include <shared_mutex>
#include <format>
#include <source_location>
#include <unordered_set>

namespace EntropyEngine {
namespace Core {
namespace Logging {

    /**
     * @brief Main logger class that manages sinks and provides logging interface
     * 
     * Logger serves as the central hub of the logging system. It receives log messages,
     * formats them using std::format, and distributes them to all registered sinks.
     * The logger coordinates multiple output destinations while maintaining thread
     * safety and performance.
     * 
     * Key features:
     * - Thread-safe: Supports concurrent logging from multiple threads
     * - Multiple sinks: Route logs to various destinations simultaneously
     * - Runtime configuration: Modify log levels and sinks dynamically
     * - Can be compiled out if needed
     * - Modern C++20: Leverages std::format for type-safe formatting
     * - Source tracking: Automatic capture of log origin locations
     * 
     * While a single global logger suffices for most applications, multiple
     * loggers can be created for subsystem-specific logging needs.
     */
    class Logger {
    private:
        std::string _name;
        std::vector<LogSinkPtr> _sinks;
        mutable std::shared_mutex _sinkMutex;
        LogLevel _minLevel = LogLevel::Trace;
        
        // Category filtering
        std::unordered_set<std::string> _disabledCategories;
        mutable std::shared_mutex _categoryMutex;

        // Global logger instance
        static Logger* s_globalLogger;
        static std::mutex s_globalMutex;
        
    public:
        explicit Logger(std::string name) : _name(std::move(name)) {}
        
        /**
         * @brief Get the global logger instance
         * 
         * Lazy-initialized on first access. Persists for program lifetime.
         * 
         * @return Reference to the global logger
         * 
         * @code
         * // Direct access
         * Logger::global().info("System", "Application started");
         * 
         * // Preferred macro usage:
         * ENTROPY_LOG_INFO("Application started");
         * @endcode
         */
        static Logger& global();
        
        /**
         * @brief Set a custom global logger
         * 
         * Replaces default logger. Useful for testing and custom implementations.
         * 
         * @param logger Pointer to the new global logger (takes ownership)
         */
        static void setGlobal(Logger* logger);
        
        /**
         * @brief Add a sink to this logger
         * 
         * Multiple sinks can be active simultaneously for various destinations.
         * 
         * @param sink The sink to add (shared ownership)
         * 
         * @code
         * auto& logger = Logger::global();
         * logger.addSink(std::make_shared<ConsoleSink>());
         * logger.addSink(std::make_shared<FileSink>("app.log"));
         * // Logs now output to both console and file
         * @endcode
         */
        void addSink(LogSinkPtr sink);
        
        /**
         * @brief Remove a sink from this logger
         * 
         * Sink remains valid but no longer receives messages.
         * 
         * @param sink The sink to remove
         */
        void removeSink(const LogSinkPtr& sink);
        
        /**
         * @brief Clear all sinks
         * 
         * Messages will be processed but not output. Useful for reconfiguration.
         */
        void clearSinks();
        
        /**
         * @brief Set minimum log level for this logger
         * 
         * Messages below this level are discarded before reaching sinks.
         * 
         * @param level The minimum level to process
         */
        void setMinLevel(LogLevel level) { _minLevel = level; }
        
        /**
         * @brief Get minimum log level
         * 
         * @return The current minimum log level
         */
        LogLevel getMinLevel() const { return _minLevel; }

        /**
         * @brief Disable logging for a specific category
         *
         * Messages from this category will be filtered out regardless of level.
         *
         * @param category The category name to disable
         *
         * @code
         * Logger::global().disableCategory("ShaderAsset");
         * Logger::global().disableCategory("ShaderService");
         * @endcode
         */
        void disableCategory(const std::string& category) {
            std::unique_lock<std::shared_mutex> lock(_categoryMutex);
            _disabledCategories.insert(category);
        }

        /**
         * @brief Re-enable logging for a specific category
         *
         * @param category The category name to enable
         */
        void enableCategory(const std::string& category) {
            std::unique_lock<std::shared_mutex> lock(_categoryMutex);
            _disabledCategories.erase(category);
        }

        /**
         * @brief Check if a category is enabled for logging
         *
         * @param category The category to check
         * @return true if the category is enabled, false if disabled
         */
        bool isCategoryEnabled(std::string_view category) const {
            std::shared_lock<std::shared_mutex> lock(_categoryMutex);
            return _disabledCategories.find(std::string(category)) == _disabledCategories.end();
        }

        /**
         * @brief Clear all disabled categories
         *
         * Re-enables all previously disabled categories.
         */
        void clearDisabledCategories() {
            std::unique_lock<std::shared_mutex> lock(_categoryMutex);
            _disabledCategories.clear();
        }
        
        /**
         * @brief Core logging function with pre-formatted message
         * 
         * Source location captured automatically using C++20.
         * 
         * @param level Message severity level
         * @param category Subsystem/module category
         * @param message The log message
         * @param location Source location (auto-captured)
         */
        void log(LogLevel level, 
                std::string_view category,
                const std::string& message,
                const std::source_location& location = std::source_location::current()) {
            
            // Early exit if level is too low
            if (level < _minLevel) return;
            
            // Early exit if category is disabled
            if (!isCategoryEnabled(category)) return;

            // Create log entry
            LogEntry entry(level, category, message, location);
            
            // Send to all sinks
            writeToSinks(entry);
        }
        
        /**
         * @brief Core logging function with format string
         * 
         * Uses std::format for type-safe message formatting.
         * 
         * @tparam Args Format argument types (deduced automatically)
         * @param level Message severity level
         * @param category Subsystem/module category
         * @param fmt Format string (std::format syntax)
         * @param args Arguments to format
         * @param location Source location (auto-captured)
         * 
         * @code
         * logger.log(LogLevel::Info, "Network", "Connected to {} on port {}", 
         *            serverName, portNumber);
         * @endcode
         */
        template<typename... Args>
        void log(LogLevel level, 
                std::string_view category,
                std::format_string<Args...> fmt,
                Args&&... args,
                const std::source_location& location = std::source_location::current()) {
            
            // Early exit if level is too low
            if (level < _minLevel) return;
            
            // Early exit if category is disabled
            if (!isCategoryEnabled(category)) return;

            // Format the message
            std::string message = std::format(fmt, std::forward<Args>(args)...);
            
            // Create log entry
            LogEntry entry(level, category, std::move(message), location);
            
            // Send to all sinks
            writeToSinks(entry);
        }
        
        /**
         * @brief Convenience methods for each log level
         * 
         * Direct methods for each severity without specifying LogLevel.
         * Supports both format strings and plain strings.
         */
        template<typename... Args>
        void trace(std::string_view category, std::format_string<Args...> fmt, Args&&... args,
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Trace, category, fmt, std::forward<Args>(args)..., loc);
        }
        
        void trace(std::string_view category, const std::string& message,
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Trace, category, message, loc);
        }
        
        template<typename... Args>
        void debug(std::string_view category, std::format_string<Args...> fmt, Args&&... args,
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Debug, category, fmt, std::forward<Args>(args)..., loc);
        }
        
        void debug(std::string_view category, const std::string& message,
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Debug, category, message, loc);
        }
        
        template<typename... Args>
        void info(std::string_view category, std::format_string<Args...> fmt, Args&&... args,
                 const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Info, category, fmt, std::forward<Args>(args)..., loc);
        }
        
        void info(std::string_view category, const std::string& message,
                 const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Info, category, message, loc);
        }
        
        template<typename... Args>
        void warning(std::string_view category, std::format_string<Args...> fmt, Args&&... args,
                    const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Warning, category, fmt, std::forward<Args>(args)..., loc);
        }
        
        void warning(std::string_view category, const std::string& message,
                    const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Warning, category, message, loc);
        }
        
        template<typename... Args>
        void error(std::string_view category, std::format_string<Args...> fmt, Args&&... args,
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Error, category, fmt, std::forward<Args>(args)..., loc);
        }
        
        void error(std::string_view category, const std::string& message,
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Error, category, message, loc);
        }
        
        template<typename... Args>
        void fatal(std::string_view category, std::format_string<Args...> fmt, Args&&... args,
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Fatal, category, fmt, std::forward<Args>(args)..., loc);
            // Fatal should flush all sinks
            flush();
        }
        
        void fatal(std::string_view category, const std::string& message,
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::Fatal, category, message, loc);
            // Fatal should flush all sinks
            flush();
        }
        
        /**
         * @brief Flush all sinks
         * 
         * Forces immediate write of buffered data. Called automatically after
         * Fatal messages.
         */
        void flush();
        
    private:
        void writeToSinks(const LogEntry& entry);
    };

} // namespace Logging
} // namespace Core
} // namespace EntropyEngine

/**
 * @brief Convenience macros for simplified logging
 * 
 * These macros provide the easiest way to add logging to your code.
 * They automatically:
 * - Use the global logger instance
 * - Capture source location information
 * - Use function name as category (non-CAT variants)
 * - Support both format strings and plain strings
 * 
 * Available variants:
 * - ENTROPY_LOG_XXX: Uses current function name as category
 * - ENTROPY_LOG_XXX_CAT: Allows explicit category specification
 * 
 * @code
 * void processData() {
 *     ENTROPY_LOG_DEBUG("Starting processing");
 *     
 *     if (data.empty()) {
 *         ENTROPY_LOG_WARNING("No data to process");
 *         return;
 *     }
 *     
 *     ENTROPY_LOG_INFO_CAT("DataProcessor", "Processing {} items", data.size());
 * }
 * @endcode
 */
#define ENTROPY_LOG_TRACE(fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().trace(__func__, fmt, ##__VA_ARGS__)

#define ENTROPY_LOG_DEBUG(fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().debug(__func__, fmt, ##__VA_ARGS__)

#define ENTROPY_LOG_INFO(fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().info(__func__, fmt, ##__VA_ARGS__)

#define ENTROPY_LOG_WARNING(fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().warning(__func__, fmt, ##__VA_ARGS__)

#define ENTROPY_LOG_ERROR(fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().error(__func__, fmt, ##__VA_ARGS__)

#define ENTROPY_LOG_FATAL(fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().fatal(__func__, fmt, ##__VA_ARGS__)

// Category-specific macros for explicit category specification
#define ENTROPY_LOG_TRACE_CAT(category, fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().trace(category, fmt, ##__VA_ARGS__)

#define ENTROPY_LOG_DEBUG_CAT(category, fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().debug(category, fmt, ##__VA_ARGS__)

#define ENTROPY_LOG_INFO_CAT(category, fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().info(category, fmt, ##__VA_ARGS__)

#define ENTROPY_LOG_WARNING_CAT(category, fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().warning(category, fmt, ##__VA_ARGS__)

#define ENTROPY_LOG_ERROR_CAT(category, fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().error(category, fmt, ##__VA_ARGS__)

#define ENTROPY_LOG_FATAL_CAT(category, fmt, ...) \
    ::EntropyEngine::Core::Logging::Logger::global().fatal(category, fmt, ##__VA_ARGS__)

