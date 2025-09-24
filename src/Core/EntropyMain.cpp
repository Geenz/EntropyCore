/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "Core/entropy_main.h"
#include "Core/EntropyApplication.h"

using EntropyEngine::Core::EntropyApplication;

extern "C" {

struct CppDelegate : EntropyEngine::Core::EntropyAppDelegate {
    EntropyAppDelegateC del{};
    EntropyApplication* app = nullptr;
    void applicationWillFinishLaunching() override {
        if (del.will_finish_launching) del.will_finish_launching((EntropyApp*)app, del.userdata);
    }
    void applicationDidFinishLaunching() override {
        if (del.did_finish_launching) del.did_finish_launching((EntropyApp*)app, del.userdata);
    }
    bool applicationShouldTerminate() override {
        if (del.should_terminate) return del.should_terminate((EntropyApp*)app, del.userdata);
        return true;
    }
    void applicationWillTerminate() override {
        if (del.will_terminate) del.will_terminate((EntropyApp*)app, del.userdata);
    }
    void applicationDidCatchUnhandledException(std::exception_ptr) override {
        if (del.did_catch_unhandled_exception) del.did_catch_unhandled_exception((EntropyApp*)app, del.userdata);
    }
};

int entropy_main_run(const EntropyMainConfig* cfg,
                     const EntropyAppDelegateC* delegate) {
    auto& app = EntropyApplication::shared();

    EntropyEngine::Core::EntropyApplicationConfig cc{};
    if (cfg) {
        cc.workerThreads = cfg->worker_threads;
        cc.installSignalHandlers = cfg->install_signal_handlers;
        cc.shutdownDeadline = std::chrono::milliseconds(cfg->shutdown_deadline_ms ? cfg->shutdown_deadline_ms : 3000);
    }
    app.configure(cc);

    CppDelegate cppDel;
    if (delegate) cppDel.del = *delegate;
    cppDel.app = &app;
    app.setDelegate(delegate ? &cppDel : nullptr);

    int rc = app.run();
    return rc;
}

void entropy_main_terminate(int code) {
    EntropyApplication::shared().terminate(code);
}

EntropyApp* entropy_main_app(void) { return (EntropyApp*)&EntropyApplication::shared(); }

} // extern "C"
