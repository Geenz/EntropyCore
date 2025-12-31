/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 */

#pragma once

#include <memory>
#include <mutex>

#include "Concurrency/WorkService.h"

namespace EntropyEngine
{
namespace Core
{
namespace Testing
{

/**
 * @brief Singleton wrapper for WorkService in tests
 *
 * WorkService should only have one instance active at a time due to
 * static thread_local variables. This wrapper ensures that constraint.
 */
class TestWorkServiceSingleton
{
private:
    static std::unique_ptr<Concurrency::WorkService> instance;
    static std::mutex mutex;

public:
    static Concurrency::WorkService& getInstance(const Concurrency::WorkService::Config& config = {}) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!instance) {
            instance = std::make_unique<Concurrency::WorkService>(config);
        }
        return *instance;
    }

    static void reset() {
        std::lock_guard<std::mutex> lock(mutex);
        if (instance) {
            instance->stop();
            instance.reset();
        }
    }

    // Prevent copying
    TestWorkServiceSingleton() = delete;
    TestWorkServiceSingleton(const TestWorkServiceSingleton&) = delete;
    TestWorkServiceSingleton& operator=(const TestWorkServiceSingleton&) = delete;
};

// Static member definitions - marked inline to avoid ODR violations in header
inline std::unique_ptr<Concurrency::WorkService> TestWorkServiceSingleton::instance = nullptr;
inline std::mutex TestWorkServiceSingleton::mutex;

}  // namespace Testing
}  // namespace Core
}  // namespace EntropyEngine
