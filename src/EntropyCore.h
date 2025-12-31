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
 * @file EntropyCore.h
 * @brief Single header that includes all EntropyCore components
 *
 * This header can be used as an alternative to C++20 modules for
 * compilers that don't fully support modules yet.
 */

// Core common utilities
#include "CoreCommon.h"

// Core object model
#include "Core/EntropyClass.h"
#include "Core/EntropyInterop.h"
#include "Core/EntropyObject.h"
#include "Core/RefObject.h"

// Type System
#include "TypeSystem/GenericHandle.h"

// Graph
#include "Graph/AcyclicNodeHandle.h"
#include "Graph/DirectedAcyclicGraph.h"

// Debug
#include "Debug/Debug.h"
#include "Debug/DebugUtilities.h"
#include "Debug/INamed.h"

// Logging
#include "Logging/ConsoleSink.h"
#include "Logging/ILogSink.h"
#include "Logging/LogEntry.h"
#include "Logging/LogLevel.h"
#include "Logging/Logger.h"

// Concurrency
#include "Concurrency/AdaptiveRankingScheduler.h"
#include "Concurrency/DirectScheduler.h"
#include "Concurrency/IConcurrencyProvider.h"
#include "Concurrency/IWorkScheduler.h"
#include "Concurrency/RandomScheduler.h"
#include "Concurrency/RoundRobinScheduler.h"
#include "Concurrency/SignalTree.h"
#include "Concurrency/SpinningDirectScheduler.h"
#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkContractHandle.h"
#include "Concurrency/WorkGraph.h"
#include "Concurrency/WorkService.h"
