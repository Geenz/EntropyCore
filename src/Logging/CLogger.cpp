/* C logger shim forwarding to C++ Logger backend */
#include "Logging/CLogger.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "Logging/LogLevel.h"
#include "Logging/Logger.h"

using ::EntropyEngine::Core::Logging::Logger;
using ::EntropyEngine::Core::Logging::LogLevel;

static LogLevel map_level(EntropyLogLevelC lvl) noexcept {
    switch (lvl) {
        case ENTROPY_LOG_TRACE_C:
            return LogLevel::Trace;
        case ENTROPY_LOG_DEBUG_C:
            return LogLevel::Debug;
        case ENTROPY_LOG_INFO_C:
            return LogLevel::Info;
        case ENTROPY_LOG_WARN_C:
            return LogLevel::Warning;
        case ENTROPY_LOG_ERROR_C:
            return LogLevel::Error;
        case ENTROPY_LOG_FATAL_C:
            return LogLevel::Fatal;
        default:
            return LogLevel::Info;
    }
}

static void vwrite_internal(EntropyLogLevelC level, const char* category, const char* fmt, va_list args) {
    if (!fmt) return;

    // Format into a dynamically-sized buffer
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = std::vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed < 0) return;

    std::string message;
    message.resize(static_cast<size_t>(needed));
    std::vsnprintf(message.data(), message.size() + 1, fmt, args);

    // Forward to Logger backend
    if (category && *category) {
        Logger::global().log(map_level(level), category, message);
    } else {
        Logger::global().log(map_level(level), "C", message);
    }
}

extern "C" {

void entropy_log_vwrite(EntropyLogLevelC level, const char* fmt, va_list args) {
    vwrite_internal(level, "C", fmt, args);
}

void entropy_log_vwrite_cat(EntropyLogLevelC level, const char* category, const char* fmt, va_list args) {
    vwrite_internal(level, category, fmt, args);
}

void entropy_log_write(EntropyLogLevelC level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vwrite_internal(level, "C", fmt, args);
    va_end(args);
}

void entropy_log_write_cat(EntropyLogLevelC level, const char* category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vwrite_internal(level, category, fmt, args);
    va_end(args);
}

}  // extern "C"
