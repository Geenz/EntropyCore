/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "Core/EntropyApplication.h"

#include <cstdlib>
#include <thread>
#include <utility>

#include "Concurrency/WorkService.h"
#include "Core/RefObject.h"
#include "Core/TimerService.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace EntropyEngine
{
namespace Core
{

EntropyApplication& EntropyApplication::shared() {
    static EntropyApplication inst;
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
        auto work = makeRef<Concurrency::WorkService>(wcfg);
        _services.registerService<Concurrency::WorkService>(std::move(work));
    }

    if (!_services.has<TimerService>()) {
        auto timer = makeRef<TimerService>();
        _services.registerService<TimerService>(std::move(timer));
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
    // Always install signal handlers
    installSignalHandlers();
#else
    // Create signal notification pipe for Unix
    if (_signalPipe[0] == -1) {
        if (pipe(_signalPipe) == 0) {
            // Set both ends non-blocking
            fcntl(_signalPipe[0], F_SETFL, O_NONBLOCK);
            fcntl(_signalPipe[1], F_SETFL, O_NONBLOCK);
        }
    }
    // Always install signal handlers
    installSignalHandlers();
#endif

    // Drive service lifecycle
    _services.loadAll();

    // Inject WorkService into TimerService before starting
    if (_services.has<TimerService>()) {
        auto timerService = _services.get<TimerService>();
        auto workService = _services.get<Concurrency::WorkService>();
        if (timerService && workService) {
            timerService->setWorkService(workService.get());
        }
    }

    if (_delegate) _delegate->applicationWillFinishLaunching();
    _services.startAll();
    if (_delegate) _delegate->applicationDidFinishLaunching();

    // Spawn dedicated signal handler thread
    // This thread waits for OS signals and handles termination requests
    // while the main thread runs the application loop at full speed
    std::jthread signalThread([this](const std::stop_token& stopToken) {
#if defined(_WIN32)
        // Windows: wait on console control events
        HANDLE ctrlH = static_cast<HANDLE>(_ctrlEvent);
        while (!stopToken.stop_requested() && !_terminateRequested.load(std::memory_order_acquire)) {
            if (ctrlH) {
                DWORD w = WaitForSingleObject(ctrlH, 100);  // 100ms timeout for responsiveness
                if (w == WAIT_OBJECT_0) {
                    auto type = _lastCtrlType.load(std::memory_order_relaxed);
                    handleConsoleSignal(type);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
#else
        // Unix: wait on signal pipe
        while (!stopToken.stop_requested() && !_terminateRequested.load(std::memory_order_acquire)) {
            if (_signalPipe[0] != -1) {
                struct pollfd pfd;
                pfd.fd = _signalPipe[0];
                pfd.events = POLLIN;
                int ret = poll(&pfd, 1, 100);  // 100ms timeout for responsiveness

                if (ret > 0 && (pfd.revents & POLLIN)) {
                    // Signal received - drain pipe and handle
                    char buf[1];
                    while (read(_signalPipe[0], buf, 1) > 0);

                    int signum = _lastSignal.load(std::memory_order_relaxed);
                    handlePosixSignal(signum);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
#endif
    });

    // Main application loop - runs at full speed with no blocking waits
    while (!_terminateRequested.load(std::memory_order_acquire)) {
        // Execute all pending main thread work from the work service
        if (auto workService = _services.get<Concurrency::WorkService>()) {
            workService->executeMainThreadWork();
        }

        // Let the application delegate run its per-frame logic
        if (_delegate) {
            _delegate->applicationMainLoop();
        }
    }

    // Signal handler thread will stop automatically via stop_token when signalThread goes out of scope

    if (_delegate) _delegate->applicationWillTerminate();
    _services.stopAll();
    _services.unloadAll();

#if defined(_WIN32)
    // Always uninstall signal handlers
    uninstallSignalHandlers();
    if (_terminateEvent) {
        CloseHandle(static_cast<HANDLE>(_terminateEvent));
        _terminateEvent = nullptr;
    }
#else
    // Always uninstall signal handlers
    uninstallSignalHandlers();
    if (_signalPipe[0] != -1) {
        close(_signalPipe[0]);
        _signalPipe[0] = -1;
    }
    if (_signalPipe[1] != -1) {
        close(_signalPipe[1]);
        _signalPipe[1] = -1;
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
namespace
{
// Free function with exact signature expected by SetConsoleCtrlHandler
static BOOL WINAPI EntropyConsoleCtrlHandler(DWORD ctrlType) {
    EntropyEngine::Core::EntropyApplication::shared().notifyConsoleSignalFromHandler(ctrlType);
    return TRUE;
}
}  // namespace

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
            return;  // ignore others
    }

    bool first = !_signalSeen.exchange(true);

    if (first) {
        // Optionally consult delegate; if vetoed, just return on first request
        bool allow = true;
        if (_delegate) {
            try {
                allow = _delegate->applicationShouldTerminate();
            } catch (...) { /* swallow in signal path */
            }
        }
        if (allow) {
            terminate(0);
        }
        // Start escalation timer after first signal regardless, to avoid hanging forever
        if (!_escalationStarted.exchange(true)) {
            auto deadline = _cfg.shutdownDeadline;
            // EntropyApplication is a static singleton - safe to capture 'this'
            std::thread([this, deadline] {
                auto endAt = std::chrono::steady_clock::now() + deadline;
                std::this_thread::sleep_until(endAt);
                if (isRunning()) {
                    // Escalate: attempt a harder exit
                    // If terminate didn't succeed yet, try again, then quick_exit.
                    terminate(1);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (isRunning()) {
                        std::quick_exit(1);
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
#else
// Unix/POSIX signal handling
namespace
{
// Signal handler - must be async-signal-safe
static void entropySigHandler(int signum) {
    EntropyEngine::Core::EntropyApplication::shared().notifyPosixSignalFromHandler(signum);
}
}  // namespace

void EntropyApplication::installSignalHandlers() {
    if (_handlersInstalled.exchange(true)) return;

    // Set up sigaction for graceful termination signals
    struct sigaction sa;
    sa.sa_handler = entropySigHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // Install handlers for common signals
    sigaction(SIGINT, &sa, nullptr);   // Ctrl+C
    sigaction(SIGTERM, &sa, nullptr);  // termination request
    sigaction(SIGHUP, &sa, nullptr);   // hangup
    sigaction(SIGQUIT, &sa, nullptr);  // quit signal

    // For fatal signals like SIGSEGV, SIGABRT - also install but allow default behavior after logging
    struct sigaction fatalSa;
    fatalSa.sa_handler = entropySigHandler;
    sigemptyset(&fatalSa.sa_mask);
    fatalSa.sa_flags = SA_RESETHAND;  // Reset to default after first signal

    sigaction(SIGABRT, &fatalSa, nullptr);  // abort
    sigaction(SIGSEGV, &fatalSa, nullptr);  // segmentation fault
    sigaction(SIGBUS, &fatalSa, nullptr);   // bus error
    sigaction(SIGFPE, &fatalSa, nullptr);   // floating point exception
    sigaction(SIGILL, &fatalSa, nullptr);   // illegal instruction
}

void EntropyApplication::uninstallSignalHandlers() {
    if (!_handlersInstalled.exchange(false)) return;

    // Restore default signal handlers
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGABRT, SIG_DFL);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    signal(SIGFPE, SIG_DFL);
    signal(SIGILL, SIG_DFL);
}

void EntropyApplication::notifyPosixSignalFromHandler(int signum) noexcept {
    _lastSignal.store(signum, std::memory_order_relaxed);
    // Write to pipe to wake up main thread (signal-safe operation)
    if (_signalPipe[1] != -1) {
        char byte = 1;
        (void)write(_signalPipe[1], &byte, 1);
    }
}

void EntropyApplication::handlePosixSignal(int signum) {
    // Map signals we care about
    bool isFatal = false;
    switch (signum) {
        case SIGINT:
        case SIGTERM:
        case SIGHUP:
        case SIGQUIT:
            break;  // Graceful termination signals
        case SIGABRT:
        case SIGSEGV:
        case SIGBUS:
        case SIGFPE:
        case SIGILL:
            isFatal = true;
            break;
        default:
            return;  // ignore others
    }

    bool first = !_signalSeen.exchange(true);

    if (first) {
        // Optionally consult delegate; if vetoed, just return on first request
        bool allow = true;
        if (_delegate && !isFatal) {
            try {
                allow = _delegate->applicationShouldTerminate();
            } catch (...) { /* swallow in signal path */
            }
        }

        if (allow || isFatal) {
            terminate(isFatal ? 1 : 0);
        }

        // Start escalation timer after first signal regardless, to avoid hanging forever
        if (!_escalationStarted.exchange(true) && !isFatal) {
            auto deadline = _cfg.shutdownDeadline;
            // EntropyApplication is a static singleton - safe to capture 'this'
            std::thread([this, deadline] {
                auto endAt = std::chrono::steady_clock::now() + deadline;
                std::this_thread::sleep_until(endAt);
                if (isRunning()) {
                    // Escalate: attempt a harder exit
                    terminate(1);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (isRunning()) {
                        std::quick_exit(1);
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

}  // namespace Core
}  // namespace EntropyEngine
