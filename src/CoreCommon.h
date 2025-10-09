/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#pragma once

/**
 * @file CoreCommon.h
 * @brief Core common utilities and debugging macros for EntropyCore
 * 
 * This header provides essential debugging utilities and common macros
 * used throughout the EntropyCore library. It includes debug assertions
 * and build configuration flags.
 */

#include <cassert>
#include <optional>
#include <string>
#include <cstdlib>

#ifdef EntropyDebug
#define ENTROPY_DEBUG_BLOCK(code) do { code } while(0)
#undef NDEBUG
#define ENTROPY_ASSERT(condition, message) assert(condition)
#else
#define ENTROPY_DEBUG_BLOCK(code) ((void)0)
#define ENTROPY_ASSERT(condition, message) ((void)0)
#endif

namespace EntropyEngine {
namespace Core {
    // Cross-platform safe environment variable getter that avoids returning raw pointers
    // and copies into std::string. Returns std::nullopt if the variable is not set.
    inline std::optional<std::string> safeGetEnv(const char* name) {
        if (!name) return std::nullopt;
#if defined(_WIN32)
        // Use secure getenv_s to query size first
        size_t required = 0;
        errno_t err = getenv_s(&required, nullptr, 0, name);
        if (err != 0 || required == 0) return std::nullopt;
        // required includes the null terminator
        std::string value;
        value.resize(required);
        size_t read = 0;
        err = getenv_s(&read, value.data(), value.size(), name);
        if (err != 0 || read == 0) return std::nullopt;
        // Trim trailing null if present
        if (!value.empty() && value.back() == '\0') value.pop_back();
        return value;
#else
        const char* v = std::getenv(name);
        if (!v) return std::nullopt;
        return std::string(v);
#endif
    }
} // namespace Core
    // Core utility namespace - currently empty but reserved for future utilities
}