/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include <Concurrency/WorkContractGroup.h>
#include <Concurrency/WorkService.h>
#include <Core/EntropyApplication.h>
#include <Logging/Logger.h>

#include <atomic>
#include <chrono>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;

/**
 * Custom App Delegate Example
 *
 * This example demonstrates:
 * 1. Creating a custom EntropyAppDelegate with applicationMainLoop()
 * 2. Using the main thread work pump to execute work on the main thread
 * 3. Coordinating between background work and main thread work
 * 4. Proper cleanup and termination
 */
class CustomAppDelegate : public EntropyAppDelegate
{
    std::shared_ptr<WorkService> workService_;
    std::unique_ptr<WorkContractGroup> backgroundGroup_;
    std::unique_ptr<WorkContractGroup> mainThreadGroup_;

    std::atomic<int> backgroundTasksCompleted_{0};
    std::atomic<int> mainThreadTasksCompleted_{0};
    std::atomic<bool> allWorkScheduled_{false};

    static constexpr int TOTAL_BACKGROUND_TASKS = 10;
    static constexpr int TOTAL_MAIN_THREAD_TASKS = 5;

public:
    void applicationWillFinishLaunching() override {
        ENTROPY_LOG_INFO("[CustomAppDelegateExample] applicationWillFinishLaunching");
    }

    void applicationDidFinishLaunching() override {
        ENTROPY_LOG_INFO("[CustomAppDelegateExample] applicationDidFinishLaunching");

        // Get the WorkService
        workService_ = EntropyApplication::shared().services().get<WorkService>();
        if (!workService_) {
            ENTROPY_LOG_ERROR("[CustomAppDelegateExample] WorkService not available!");
            EntropyApplication::shared().terminate(1);
            return;
        }

        // Create work groups
        backgroundGroup_ = std::make_unique<WorkContractGroup>(64, "BackgroundGroup");
        mainThreadGroup_ = std::make_unique<WorkContractGroup>(64, "MainThreadGroup");

        // Register groups with the work service
        auto status1 = workService_->addWorkContractGroup(backgroundGroup_.get());
        auto status2 = workService_->addWorkContractGroup(mainThreadGroup_.get());

        if (status1 != WorkService::GroupOperationStatus::Added &&
            status1 != WorkService::GroupOperationStatus::Exists) {
            ENTROPY_LOG_ERROR("[CustomAppDelegateExample] Failed to register background group");
            EntropyApplication::shared().terminate(1);
            return;
        }

        if (status2 != WorkService::GroupOperationStatus::Added &&
            status2 != WorkService::GroupOperationStatus::Exists) {
            ENTROPY_LOG_ERROR("[CustomAppDelegateExample] Failed to register main thread group");
            EntropyApplication::shared().terminate(1);
            return;
        }

        ENTROPY_LOG_INFO("[CustomAppDelegateExample] Scheduling work...");
        scheduleWork();
    }

    void scheduleWork() {
        // Schedule background work (runs on worker threads)
        for (int i = 0; i < TOTAL_BACKGROUND_TASKS; ++i) {
            auto handle = backgroundGroup_->createContract([this, i]() noexcept {
                // Simulate some work
                std::this_thread::sleep_for(std::chrono::milliseconds(50));

                int completed = ++backgroundTasksCompleted_;
                ENTROPY_LOG_INFO(
                    std::format("  [Background] Task {} completed ({}/{})", i, completed, TOTAL_BACKGROUND_TASKS));

                // When background work completes, schedule a main thread task
                scheduleMainThreadTask(i);
            });

            if (handle.valid()) {
                handle.schedule();
            }
        }

        allWorkScheduled_.store(true, std::memory_order_release);
        ENTROPY_LOG_INFO(
            std::format("[CustomAppDelegateExample] Scheduled {} background tasks", TOTAL_BACKGROUND_TASKS));
    }

    void scheduleMainThreadTask(int taskId) {
        // Schedule work that MUST run on the main thread
        auto handle = mainThreadGroup_->createContract(
            [this, taskId]() noexcept {
                // This executes on the main thread via executeMainThreadWork()
                int completed = ++mainThreadTasksCompleted_;
                ENTROPY_LOG_INFO(std::format("  [MainThread] Task {} completed ({}/{})", taskId, completed,
                                             TOTAL_MAIN_THREAD_TASKS));

                // Check if all work is done
                checkCompletion();
            },
            ExecutionType::MainThread  // Force main thread execution
        );

        if (handle.valid()) {
            handle.schedule();
        }
    }

    void checkCompletion() {
        // Check if all work is complete
        if (backgroundTasksCompleted_.load() >= TOTAL_BACKGROUND_TASKS &&
            mainThreadTasksCompleted_.load() >= TOTAL_BACKGROUND_TASKS) {
            ENTROPY_LOG_INFO("[CustomAppDelegateExample] All work completed! Terminating...");
            EntropyApplication::shared().terminate(0);
        }
    }

    void applicationMainLoop() override {
        // This is called regularly by EntropyApplication's main loop
        // You can use this to update your application state, process events, etc.

        // For this example, we'll just log occasionally to show it's being called
        static auto lastLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 1) {
            if (allWorkScheduled_.load(std::memory_order_acquire)) {
                ENTROPY_LOG_DEBUG(
                    std::format("[CustomAppDelegateExample] applicationMainLoop - Background: {}/{}, MainThread: {}/{}",
                                backgroundTasksCompleted_.load(), TOTAL_BACKGROUND_TASKS,
                                mainThreadTasksCompleted_.load(), TOTAL_BACKGROUND_TASKS));
            }
            lastLog = now;
        }
    }

    bool applicationShouldTerminate() override {
        ENTROPY_LOG_INFO("[CustomAppDelegateExample] applicationShouldTerminate -> true");
        return true;
    }

    void applicationWillTerminate() override {
        ENTROPY_LOG_INFO("[CustomAppDelegateExample] applicationWillTerminate");

        // Clean up work groups
        if (workService_) {
            if (backgroundGroup_) {
                workService_->removeWorkContractGroup(backgroundGroup_.get());
            }
            if (mainThreadGroup_) {
                workService_->removeWorkContractGroup(mainThreadGroup_.get());
            }
        }

        mainThreadGroup_.reset();
        backgroundGroup_.reset();
        workService_.reset();

        ENTROPY_LOG_INFO(std::format("[CustomAppDelegateExample] Final stats - Background: {}, MainThread: {}",
                                     backgroundTasksCompleted_.load(), mainThreadTasksCompleted_.load()));
    }

    void applicationDidCatchUnhandledException(std::exception_ptr) override {
        ENTROPY_LOG_ERROR("[CustomAppDelegateExample] Unhandled exception observed");
    }
};

int main() {
    ENTROPY_LOG_INFO("[CustomAppDelegateExample] Starting...");

    auto& app = EntropyApplication::shared();

    // Configure the application
    EntropyApplicationConfig config;
    config.workerThreads = 4;  // Use 4 worker threads
    config.shutdownDeadline = std::chrono::milliseconds(5000);

    app.configure(config);

    // Create and set our custom delegate
    CustomAppDelegate delegate;
    app.setDelegate(&delegate);

    ENTROPY_LOG_INFO("[CustomAppDelegateExample] Starting app.run()");
    int exitCode = app.run();

    ENTROPY_LOG_INFO(std::format("[CustomAppDelegateExample] Exited with code {}", exitCode));
    return exitCode;
}
