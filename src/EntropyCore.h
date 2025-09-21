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
#include "ServiceLocator.h"

// Core object model
#include "Core/EntropyClass.h"
#include "Core/EntropyObject.h"
#include "Core/RefObject.h"
#include "Core/EntropyInterop.h"

// Type System
#include "TypeSystem/GenericHandle.h"

// Graph
#include "Graph/AcyclicNodeHandle.h"
#include "Graph/DirectedAcyclicGraph.h"

// Debug
#include "Debug/INamed.h"
#include "Debug/DebugUtilities.h"
#include "Debug/Profiling.h"
#include "Debug/Debug.h"

// Logging
#include "Logging/LogLevel.h"
#include "Logging/LogEntry.h"
#include "Logging/ILogSink.h"
#include "Logging/ConsoleSink.h"
#include "Logging/Logger.h"

// Concurrency
#include "Concurrency/WorkContractHandle.h"
#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkGraph.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/SignalTree.h"
#include "Concurrency/IConcurrencyProvider.h"
#include "Concurrency/IWorkScheduler.h"
#include "Concurrency/DirectScheduler.h"
#include "Concurrency/SpinningDirectScheduler.h"
#include "Concurrency/AdaptiveRankingScheduler.h"
#include "Concurrency/RandomScheduler.h"
#include "Concurrency/RoundRobinScheduler.h"