/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Engine project.
 */

/**
 * @file IConcurrencyProvider.h
 * @brief Interface for thread pool providers that execute work from contract groups
 *
 * This file defines the IConcurrencyProvider interface, which provides the communication
 * protocol between WorkContractGroups and their execution providers (like WorkService).
 * It enables efficient thread wake-up and lifecycle management without creating tight
 * coupling between the scheduling and execution layers.
 */

#pragma once

namespace EntropyEngine
{
namespace Core
{
namespace Concurrency
{

class WorkContractGroup;

/**
 * @brief Interface for concurrency providers that execute work from WorkContractGroups
 *
 * This interface allows WorkContractGroups to notify their associated concurrency
 * provider (like WorkService) when work becomes available, enabling efficient
 * wake-up of waiting threads without creating direct dependencies.
 *
 * Implementers of this interface are responsible for executing work from the
 * groups they manage, and groups will call these methods to provide hints about
 * work availability.
 *
 * The key insight here is the inversion of control: instead of providers polling
 * groups for work (inefficient), groups push notifications when work arrives.
 * This enables sleeping threads to wake immediately when work is available, rather
 * than spinning or using fixed sleep intervals.
 *
 * Common implementations:
 * - WorkService: The main thread pool that manages multiple groups
 * - SingleThreadedProvider: Executes work on a dedicated thread (testing)
 * - ImmediateProvider: Executes work synchronously (debugging)
 *
 * Thread safety: All methods must be thread-safe as they may be called concurrently
 * from multiple WorkContractGroups.
 *
 * @code
 * // Implementing a custom provider
 * class MyProvider : public IConcurrencyProvider {
 *     std::condition_variable cv;
 *     std::mutex mutex;
 *
 *     void notifyWorkAvailable(WorkContractGroup* group) override {
 *         std::lock_guard<std::mutex> lock(mutex);
 *         cv.notify_one(); // Wake a thread to check for work
 *     }
 *
 *     void notifyGroupDestroyed(WorkContractGroup* group) override {
 *         // Remove from internal group list
 *         removeGroup(group);
 *     }
 * };
 * @endcode
 */
class IConcurrencyProvider
{
public:
    virtual ~IConcurrencyProvider() = default;

    /**
     * @brief Notifies the provider that work may be available
     *
     * Called by WorkContractGroup when new work is scheduled. The provider
     * should wake up any waiting threads to check for work. This is just a
     * hint - the work may have already been consumed by the time a thread
     * wakes up.
     *
     * @param group The group that has new work available (optional, for routing)
     */
    virtual void notifyWorkAvailable(WorkContractGroup* group = nullptr) = 0;

    /**
     * @brief Called when a group is being destroyed
     *
     * Allows the provider to clean up any references to the group.
     * After this call, the provider must not access the group pointer.
     *
     * @param group The group being destroyed
     */
    virtual void notifyGroupDestroyed(WorkContractGroup* group) = 0;

    /**
     * @brief Notifies the provider that main thread work may be available
     *
     * Called when ExecutionType::MainThread work is scheduled. Unlike regular work
     * which worker threads grab, main thread work must be explicitly pumped by
     * calling executeMainThreadWork(). Override to handle main thread notifications
     * differently (e.g., post to UI event queue).
     *
     * @param group The group that has new main thread work available (optional)
     *
     * @code
     * void notifyMainThreadWorkAvailable(WorkContractGroup* group) override {
     *     // Post event to UI thread's message queue
     *     PostMessage(hWnd, WM_MAIN_THREAD_WORK, 0, 0);
     * }
     * @endcode
     */
    virtual void notifyMainThreadWorkAvailable(WorkContractGroup* group = nullptr) {
        // Default: treat same as regular work
        notifyWorkAvailable(group);
    }
};

}  // namespace Concurrency
}  // namespace Core
}  // namespace EntropyEngine
