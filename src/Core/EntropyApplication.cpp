/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "Core/EntropyApplication.h"
#include "Concurrency/WorkService.h"
#include <utility>
#include <thread>
#include <cstdlib>
#include <memory>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace EntropyEngine { namespace Core {


EntropyApplication& EntropyApplication::shared() {
    return *sharedPtr();
}

std::shared_ptr<EntropyApplication> EntropyApplication::sharedPtr() {
    static std::shared_ptr<EntropyApplication> inst{ std::shared_ptr<EntropyApplication>(new EntropyApplication()) };
    return inst;
}

EntropyApplication::EntropyApplication() = default;

void EntropyApplication::configure(const EntropyApplicationConfig& cfg) {
    _cfg = cfg;
}

void EntropyApplication::setDelegate(EntropyAppDelegate* del) {
    _delegate = del;
}

void EntropyApplication::ensureCoreServices() {
    if (!_services.has<Concurrency::WorkService>()) {
        Concurrency::WorkService::Config wcfg{};
        wcfg.threadCount = static_cast<uint32_t>(_cfg.workerThreads);
        auto work = std::make_shared<Concurrency::WorkService>(wcfg);
        _services.registerService<Concurrency::WorkService>(work);
    }
}

int EntropyApplication::run() {
    if (_running.load()) return _exitCode.load();
    _running.store(true);
    _terminateRequested.store(false);
    _exitCode.store(0);

    ensureCoreServices();

#if defined(_WIN32)
    if (_terminateEvent == nullptr) {
        // Manual-reset event signaled by terminate()
        _terminateEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    if (_cfg.installSignalHandlers) {
        installSignalHandlers();
    }
#endif

    // Drive service lifecycle
    _services.loadAll();
    if (_delegate) _delegate->applicationWillFinishLaunching();
    _services.startAll();
    if (_delegate) _delegate->applicationDidFinishLaunching();

    // Wait until termination requested
#if defined(_WIN32)
    {
        HANDLE handles[2];
        DWORD count = 0;
        HANDLE termH = static_cast<HANDLE>(_terminateEvent);
        if (termH) { handles[count++] = termH; }
        HANDLE ctrlH = _cfg.installSignalHandlers && _ctrlEvent ? static_cast<HANDLE>(_ctrlEvent) : nullptr;
        if (ctrlH) { handles[count++] = ctrlH; }
        // Always ensure we have at least the terminate event
        if (count == 0) {
            // Fallback to condition_variable if for some reason terminate event is missing
            std::unique_lock<std::mutex> lk(_loopMutex);
            _loopCv.wait(lk, [&]{ return _terminateRequested.load(std::memory_order_acquire); });
        } else {
            for (;;) {
                DWORD w = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0) {
                    // terminateEvent signaled
                    break;
                }
                if (count >= 2 && w == WAIT_OBJECT_0 + 1) {
                    auto type = _lastCtrlType.load(std::memory_order_relaxed);
                    handleConsoleSignal(type);
                    // Continue waiting afterwards
                    continue;
                }
            }
        }
    }
#else
    {
        std::unique_lock<std::mutex> lk(_loopMutex);
        _loopCv.wait(lk, [&]{ return _terminateRequested.load(std::memory_order_acquire); });
    }
#endif

    if (_delegate) _delegate->applicationWillTerminate();
    _services.stopAll();
    _services.unloadAll();

#if defined(_WIN32)
    if (_cfg.installSignalHandlers) {
        uninstallSignalHandlers();
    }
    if (_terminateEvent) {
        CloseHandle(static_cast<HANDLE>(_terminateEvent));
        _terminateEvent = nullptr;
    }
#endif

    _running.store(false);
    return _exitCode.load();
}

void EntropyApplication::terminate(int code) {
    _exitCode.store(code);
    _terminateRequested.store(true, std::memory_order_release);
#if defined(_WIN32)
    // Signal terminate event so Windows wait loop wakes
    HANDLE th = static_cast<HANDLE>(_terminateEvent);
    if (th) SetEvent(th);
#endif
    // Wake the wait in run() for non-Windows path (noop on Windows)
    _loopCv.notify_all();
}
#if defined(_WIN32)
namespace {
    // Free function with exact signature expected by SetConsoleCtrlHandler
    static BOOL WINAPI EntropyConsoleCtrlHandler(DWORD ctrlType) {
        EntropyEngine::Core::EntropyApplication::shared().notifyConsoleSignalFromHandler(ctrlType);
        return TRUE;
    }
}

void EntropyApplication::installSignalHandlers() {
    if (_handlersInstalled.exchange(true)) return;
    // Create auto-reset event to queue console signals
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    _ctrlEvent = ev;
    // Register console control handler; processing happens on the main wait loop
    SetConsoleCtrlHandler(EntropyConsoleCtrlHandler, TRUE);
}

void EntropyApplication::uninstallSignalHandlers() {
    if (!_handlersInstalled.exchange(false)) return;
    SetConsoleCtrlHandler(EntropyConsoleCtrlHandler, FALSE);
    HANDLE h = static_cast<HANDLE>(_ctrlEvent);
    if (h) {
        // Ensure any pending waits can complete, then close
        SetEvent(h);
        CloseHandle(h);
        _ctrlEvent = nullptr;
    }
}

void EntropyApplication::notifyConsoleSignalFromHandler(unsigned long ctrlType) noexcept {
    HANDLE h = static_cast<HANDLE>(_ctrlEvent);
    _lastCtrlType.store(ctrlType, std::memory_order_relaxed);
    if (h) SetEvent(h);
}

void EntropyApplication::handleConsoleSignal(unsigned long ctrlType) {
    // Map control types we care about
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            break;
        default:
            return; // ignore others
    }

    bool first = !_signalSeen.exchange(true);

    if (first) {
        // Optionally consult delegate; if vetoed, just return on first request
        bool allow = true;
        if (_delegate) {
            try { allow = _delegate->applicationShouldTerminate(); }
            catch (...) { /* swallow in signal path */ }
        }
        if (allow) {
            terminate(0);
        }
        // Start escalation timer after first signal regardless, to avoid hanging forever
        if (!_escalationStarted.exchange(true)) {
            auto deadline = _cfg.shutdownDeadline;
            std::weak_ptr<EntropyApplication> weak = EntropyApplication::sharedPtr();
            std::thread([weak, deadline]{
                auto endAt = std::chrono::steady_clock::now() + deadline;
                std::this_thread::sleep_until(endAt);
                if (auto sp = weak.lock()) {
                    if (sp->isRunning()) {
                        // Escalate: attempt a harder exit
                        // If terminate didn't succeed yet, try again, then quick_exit.
                        sp->terminate(1);
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        if (sp->isRunning()) {
                            std::quick_exit(1);
                        }
                    }
                }
            }).detach();
        }
    } else {
        // Subsequent signal: escalate immediately
        if (_running.load()) {
            terminate(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (_running.load()) {
                std::quick_exit(1);
            }
        }
    }
}
#endif


}} // namespace EntropyEngine::Core
