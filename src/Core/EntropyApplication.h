/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <condition_variable>
#include "Core/EntropyServiceRegistry.h"

#if defined(_WIN32)
#endif

namespace EntropyEngine { namespace Core {


class EntropyAppDelegate {
public:
    virtual ~EntropyAppDelegate() = default;
    virtual void applicationWillFinishLaunching() {}
    virtual void applicationDidFinishLaunching() {}
    virtual bool applicationShouldTerminate() { return true; }
    virtual void applicationWillTerminate() {}
    virtual void applicationDidCatchUnhandledException(std::exception_ptr) {}
};

struct EntropyApplicationConfig {
    size_t workerThreads = 0; // 0 => auto
    bool installSignalHandlers = false; // MVP: off by default for safety
    std::chrono::milliseconds shutdownDeadline{3000};
};

class EntropyApplication {
public:
    static EntropyApplication& shared();

    void configure(const EntropyApplicationConfig& cfg);
    void setDelegate(EntropyAppDelegate* del);

    // Lifecycle
    int run();                 // blocks until termination
    void terminate(int code);  // thread-safe, idempotent

    // Services access
    EntropyServiceRegistry& services() { return _services; }

    int exitCode() const { return _exitCode.load(); }

#if defined(_WIN32)
    // Exposed for console control handler forwarder
    void handleConsoleSignal(unsigned long ctrlType);
#endif

private:
    EntropyApplication();
    void ensureCoreServices();

#if defined(_WIN32)
    // Windows console control handling
    void installSignalHandlers();
    void uninstallSignalHandlers();
#endif

    // Fields
    EntropyApplicationConfig _cfg{};
    EntropyAppDelegate* _delegate{nullptr};

    EntropyServiceRegistry _services;

    std::atomic<bool> _running{false};
    std::atomic<bool> _terminateRequested{false};
    std::atomic<int> _exitCode{0};

#if defined(_WIN32)
    std::atomic<bool> _handlersInstalled{false};
    std::atomic<bool> _signalSeen{false};
    std::atomic<bool> _escalationStarted{false};
#endif

    // Inline wait primitives (replacing EntropyRunLoop)
    std::mutex _loopMutex;
    std::condition_variable _loopCv;
};

}} // namespace EntropyEngine::Core
