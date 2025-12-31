/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "Logger.h"

#include <algorithm>

#include "ConsoleSink.h"

namespace EntropyEngine
{
namespace Core
{
namespace Logging
{

// Static members
Logger* Logger::s_globalLogger = nullptr;
std::mutex Logger::s_globalMutex;

Logger& Logger::global() {
    std::lock_guard<std::mutex> lock(s_globalMutex);
    if (!s_globalLogger) {
        s_globalLogger = new Logger("Global");
        // Add default console sink
        s_globalLogger->addSink(std::make_shared<ConsoleSink>());
    }
    return *s_globalLogger;
}

void Logger::setGlobal(Logger* logger) {
    std::lock_guard<std::mutex> lock(s_globalMutex);
    if (s_globalLogger && s_globalLogger != logger) {
        delete s_globalLogger;
    }
    s_globalLogger = logger;
}

void Logger::addSink(LogSinkPtr sink) {
    std::unique_lock<std::shared_mutex> lock(_sinkMutex);
    _sinks.push_back(std::move(sink));
}

void Logger::removeSink(const LogSinkPtr& sink) {
    std::unique_lock<std::shared_mutex> lock(_sinkMutex);
    _sinks.erase(std::remove(_sinks.begin(), _sinks.end(), sink), _sinks.end());
}

void Logger::clearSinks() {
    std::unique_lock<std::shared_mutex> lock(_sinkMutex);
    _sinks.clear();
}

void Logger::flush() {
    std::shared_lock<std::shared_mutex> lock(_sinkMutex);
    for (auto& sink : _sinks) {
        sink->flush();
    }
}

void Logger::writeToSinks(const LogEntry& entry) {
    std::shared_lock<std::shared_mutex> lock(_sinkMutex);

    for (auto& sink : _sinks) {
        if (sink->shouldLog(entry.level)) {
            sink->write(entry);
        }
    }

    // Auto-flush for error and fatal levels
    if (entry.level >= LogLevel::Error) {
        for (auto& sink : _sinks) {
            sink->flush();
        }
    }
}

}  // namespace Logging
}  // namespace Core
}  // namespace EntropyEngine
