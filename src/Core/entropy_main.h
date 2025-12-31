/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct EntropyApp EntropyApp;

// Delegate callbacks (executed on main thread)
typedef struct EntropyAppDelegateC
{
    void (*will_finish_launching)(EntropyApp* app, void* userdata);
    void (*did_finish_launching)(EntropyApp* app, void* userdata);
    bool (*should_terminate)(EntropyApp* app, void* userdata);
    void (*will_terminate)(EntropyApp* app, void* userdata);
    void (*did_catch_unhandled_exception)(EntropyApp* app, void* userdata);
    void* userdata;
} EntropyAppDelegateC;

// Runtime configuration for bootstrap
typedef struct EntropyMainConfig
{
    uint32_t worker_threads;        // 0 => auto
    uint32_t shutdown_deadline_ms;  // graceful drain window
} EntropyMainConfig;

// Bootstrap / run
int entropy_main_run(const EntropyMainConfig* cfg, const EntropyAppDelegateC* delegate);

// Request termination from any thread
void entropy_main_terminate(int code);

// Accessors (valid during run)
EntropyApp* entropy_main_app(void);

#ifdef __cplusplus
}  // extern "C"
#endif
