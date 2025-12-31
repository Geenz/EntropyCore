#pragma once

// C-compatible logging shim that forwards to EntropyCore's C++ logger backend.
// Provides printf-style logging macros that can be used from both C and C++.

#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif

// C-visible log levels (keep values in sync with Logging::LogLevel)
typedef enum EntropyLogLevelC
{
    ENTROPY_LOG_TRACE_C = 0,
    ENTROPY_LOG_DEBUG_C = 1,
    ENTROPY_LOG_INFO_C = 2,
    ENTROPY_LOG_WARN_C = 3,
    ENTROPY_LOG_ERROR_C = 4,
    ENTROPY_LOG_FATAL_C = 5
} EntropyLogLevelC;

// Core C APIs (printf-style)
void entropy_log_write(EntropyLogLevelC level, const char* fmt, ...);
void entropy_log_write_cat(EntropyLogLevelC level, const char* category, const char* fmt, ...);

// va_list variants
void entropy_log_vwrite(EntropyLogLevelC level, const char* fmt, va_list args);
void entropy_log_vwrite_cat(EntropyLogLevelC level, const char* category, const char* fmt, va_list args);

#ifdef __cplusplus
}  // extern "C"
#endif

// ------------------------------------------------------------
// Macros usable from BOTH C and C++ (printf-style)
// By default, non-category macros use __func__ as the category.
// ------------------------------------------------------------
#ifndef ENTROPY_LOG_CATEGORY_DEFAULT
#define ENTROPY_LOG_CATEGORY_DEFAULT __func__
#endif

#define ENTROPY_LOG_TRACE_F(fmt, ...) \
    entropy_log_write_cat(ENTROPY_LOG_TRACE_C, ENTROPY_LOG_CATEGORY_DEFAULT, (fmt), ##__VA_ARGS__)
#define ENTROPY_LOG_DEBUG_F(fmt, ...) \
    entropy_log_write_cat(ENTROPY_LOG_DEBUG_C, ENTROPY_LOG_CATEGORY_DEFAULT, (fmt), ##__VA_ARGS__)
#define ENTROPY_LOG_INFO_F(fmt, ...) \
    entropy_log_write_cat(ENTROPY_LOG_INFO_C, ENTROPY_LOG_CATEGORY_DEFAULT, (fmt), ##__VA_ARGS__)
#define ENTROPY_LOG_WARNING_F(fmt, ...) \
    entropy_log_write_cat(ENTROPY_LOG_WARN_C, ENTROPY_LOG_CATEGORY_DEFAULT, (fmt), ##__VA_ARGS__)
#define ENTROPY_LOG_ERROR_F(fmt, ...) \
    entropy_log_write_cat(ENTROPY_LOG_ERROR_C, ENTROPY_LOG_CATEGORY_DEFAULT, (fmt), ##__VA_ARGS__)
#define ENTROPY_LOG_FATAL_F(fmt, ...) \
    entropy_log_write_cat(ENTROPY_LOG_FATAL_C, ENTROPY_LOG_CATEGORY_DEFAULT, (fmt), ##__VA_ARGS__)

#define ENTROPY_LOG_TRACE_CAT_F(cat, fmt, ...) entropy_log_write_cat(ENTROPY_LOG_TRACE_C, (cat), (fmt), ##__VA_ARGS__)
#define ENTROPY_LOG_DEBUG_CAT_F(cat, fmt, ...) entropy_log_write_cat(ENTROPY_LOG_DEBUG_C, (cat), (fmt), ##__VA_ARGS__)
#define ENTROPY_LOG_INFO_CAT_F(cat, fmt, ...) entropy_log_write_cat(ENTROPY_LOG_INFO_C, (cat), (fmt), ##__VA_ARGS__)
#define ENTROPY_LOG_WARNING_CAT_F(cat, fmt, ...) entropy_log_write_cat(ENTROPY_LOG_WARN_C, (cat), (fmt), ##__VA_ARGS__)
#define ENTROPY_LOG_ERROR_CAT_F(cat, fmt, ...) entropy_log_write_cat(ENTROPY_LOG_ERROR_C, (cat), (fmt), ##__VA_ARGS__)
#define ENTROPY_LOG_FATAL_CAT_F(cat, fmt, ...) entropy_log_write_cat(ENTROPY_LOG_FATAL_C, (cat), (fmt), ##__VA_ARGS__)
