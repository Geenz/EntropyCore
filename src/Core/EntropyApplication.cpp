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

namespace EntropyEngine { namespace Core {

EntropyApplication& EntropyApplication::shared() {
    static EntropyApplication instance;
    return instance;
}

EntropyApplication::EntropyApplication() = default;

void EntropyApplication::configure(const EntropyApplicationConfig& cfg) {
    _cfg = cfg;
}

void EntropyApplication::setDelegate(EntropyAppDelegate* del) {
    _delegate = del;
}

void EntropyApplication::ensureCoreServices() {
    if (!_services.has("com.entropy.core.work")) {
        Concurrency::WorkService::Config wcfg{};
        wcfg.threadCount = static_cast<uint32_t>(_cfg.workerThreads);
        auto work = std::make_shared<Concurrency::WorkService>(wcfg);
        _services.registerService(work);
    }
}

int EntropyApplication::run() {
    if (_running.load()) return _exitCode.load();
    _running.store(true);
    _terminateRequested.store(false);
    _exitCode.store(0);

    ensureCoreServices();

    // Drive service lifecycle
    _services.loadAll();
    if (_delegate) _delegate->applicationWillFinishLaunching();
    _services.startAll();
    if (_delegate) _delegate->applicationDidFinishLaunching();

    // Wait until termination requested
    {
        std::unique_lock<std::mutex> lk(_loopMutex);
        _loopCv.wait(lk, [&]{ return _terminateRequested.load(std::memory_order_acquire); });
    }

    if (_delegate) _delegate->applicationWillTerminate();
    _services.stopAll();
    _services.unloadAll();

    _running.store(false);
    return _exitCode.load();
}

void EntropyApplication::terminate(int code) {
    _exitCode.store(code);
    _terminateRequested.store(true, std::memory_order_release);
    // Wake the wait in run()
    _loopCv.notify_all();
}


}} // namespace EntropyEngine::Core
