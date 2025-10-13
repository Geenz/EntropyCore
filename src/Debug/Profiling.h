/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file Profiling.h
 * @brief Tracy profiler integration for performance analysis
 * 
 * This file provides Entropy-specific macros wrapping Tracy profiler functionality.
 * When TRACY_ENABLE is defined, full profiling is available. Otherwise, macros
 * compile to nothing for zero overhead.
 */

#pragma once

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

#include <string_view>
#include <cstring>

namespace EntropyEngine {
namespace Core {
namespace Debug {

    /**
     * @brief Profiling macros that wrap Tracy functionality
     * 
     * Provides zone profiling, frame marking, memory tracking, GPU profiling,
     * lock profiling, and value plotting. Zero overhead when disabled.
     * 
     * @code
     * void expensiveFunction() {
     *     ENTROPY_PROFILE_ZONE();  // Measures entire function
     *     
     *     {
     *         ENTROPY_PROFILE_ZONE_N("Data Processing");
     *         processData();
     *     }
     *     
     *     {
     *         ENTROPY_PROFILE_ZONE_NC("Rendering", ProfileColors::Rendering);
     *         renderScene();
     *     }
     * }
     * @endcode
     */

    // Zone profiling - measure execution time of scopes
    #ifdef TRACY_ENABLE
        #define ENTROPY_PROFILE_ZONE() ZoneScoped
        #define ENTROPY_PROFILE_ZONE_N(name) ZoneScopedN(name)
        #define ENTROPY_PROFILE_ZONE_C(color) ZoneScopedC(color)
        #define ENTROPY_PROFILE_ZONE_NC(name, color) ZoneScopedNC(name, color)
        
        // Dynamic zone naming - runtime-determined zone names
        #define ENTROPY_PROFILE_ZONE_TEXT(text, size) ZoneText(text, size)
        #define ENTROPY_PROFILE_ZONE_NAME(name, size) ZoneName(name, size)
        
        // Frame marking - track frame boundaries and timing
        #define ENTROPY_PROFILE_FRAME_MARK() FrameMark
        #define ENTROPY_PROFILE_FRAME_MARK_N(name) FrameMarkNamed(name)
        #define ENTROPY_PROFILE_FRAME_MARK_START(name) FrameMarkStart(name)
        #define ENTROPY_PROFILE_FRAME_MARK_END(name) FrameMarkEnd(name)
        
        // Memory profiling - track allocations and memory usage
        #define ENTROPY_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
        #define ENTROPY_PROFILE_FREE(ptr) TracyFree(ptr)
        #define ENTROPY_PROFILE_ALLOC_N(ptr, size, name) TracyAllocN(ptr, size, name)
        #define ENTROPY_PROFILE_FREE_N(ptr, name) TracyFreeN(ptr, name)
        
        // GPU profiling - measure graphics operation timing
        #define ENTROPY_PROFILE_GPU_ZONE(name) TracyGpuZone(name)
        #define ENTROPY_PROFILE_GPU_ZONE_C(name, color) TracyGpuZoneC(name, color)
        #define ENTROPY_PROFILE_GPU_COLLECT() TracyGpuCollect
        
        // Lock profiling - identify synchronization contention
        #define ENTROPY_PROFILE_LOCKABLE(type, varname) TracyLockable(type, varname)
        #define ENTROPY_PROFILE_LOCKABLE_N(type, varname, desc) TracyLockableN(type, varname, desc)
        #define ENTROPY_PROFILE_SHARED_LOCKABLE(type, varname) TracySharedLockable(type, varname)
        #define ENTROPY_PROFILE_SHARED_LOCKABLE_N(type, varname, desc) TracySharedLockableN(type, varname, desc)
        
        // Plot values - graph numeric values over time
        #define ENTROPY_PROFILE_PLOT(name, val) TracyPlot(name, val)
        #define ENTROPY_PROFILE_PLOT_F(name, val) TracyPlotF(name, val)
        #define ENTROPY_PROFILE_PLOT_I(name, val) TracyPlotI(name, val)
        #define ENTROPY_PROFILE_PLOT_CONFIG(name, type) TracyPlotConfig(name, type)
        
        // Messages - add text annotations to profiler timeline
        #define ENTROPY_PROFILE_MESSAGE(txt, size) TracyMessage(txt, size)
        #define ENTROPY_PROFILE_MESSAGE_L(txt) TracyMessageL(txt)
        #define ENTROPY_PROFILE_MESSAGE_C(txt, size, color) TracyMessageC(txt, size, color)
        #define ENTROPY_PROFILE_MESSAGE_LC(txt, color) TracyMessageLC(txt, color)
        
        // App info - set application metadata
        #define ENTROPY_PROFILE_APP_INFO(txt, size) TracyAppInfo(txt, size)
        
    #else
        // No-op versions when Tracy is disabled - zero overhead
        #define ENTROPY_PROFILE_ZONE() 
        #define ENTROPY_PROFILE_ZONE_N(name)
        #define ENTROPY_PROFILE_ZONE_C(color)
        #define ENTROPY_PROFILE_ZONE_NC(name, color)
        
        #define ENTROPY_PROFILE_ZONE_TEXT(text, size)
        #define ENTROPY_PROFILE_ZONE_NAME(name, size)
        
        #define ENTROPY_PROFILE_FRAME_MARK()
        #define ENTROPY_PROFILE_FRAME_MARK_N(name)
        #define ENTROPY_PROFILE_FRAME_MARK_START(name)
        #define ENTROPY_PROFILE_FRAME_MARK_END(name)
        
        #define ENTROPY_PROFILE_ALLOC(ptr, size)
        #define ENTROPY_PROFILE_FREE(ptr)
        #define ENTROPY_PROFILE_ALLOC_N(ptr, size, name)
        #define ENTROPY_PROFILE_FREE_N(ptr, name)
        
        #define ENTROPY_PROFILE_GPU_ZONE(name)
        #define ENTROPY_PROFILE_GPU_ZONE_C(name, color)
        #define ENTROPY_PROFILE_GPU_COLLECT()
        
        #define ENTROPY_PROFILE_LOCKABLE(type, varname) type varname
        #define ENTROPY_PROFILE_LOCKABLE_N(type, varname, desc) type varname
        #define ENTROPY_PROFILE_SHARED_LOCKABLE(type, varname) type varname
        #define ENTROPY_PROFILE_SHARED_LOCKABLE_N(type, varname, desc) type varname
        
        #define ENTROPY_PROFILE_PLOT(name, val)
        #define ENTROPY_PROFILE_PLOT_F(name, val)
        #define ENTROPY_PROFILE_PLOT_I(name, val)
        #define ENTROPY_PROFILE_PLOT_CONFIG(name, type)
        
        #define ENTROPY_PROFILE_MESSAGE(txt, size)
        #define ENTROPY_PROFILE_MESSAGE_L(txt)
        #define ENTROPY_PROFILE_MESSAGE_C(txt, size, color)
        #define ENTROPY_PROFILE_MESSAGE_LC(txt, color)
        
        #define ENTROPY_PROFILE_APP_INFO(txt, size)
    #endif

    /**
     * @brief Helper class for profiling with runtime zone naming
     * 
     * Use when zone names are determined at runtime. For compile-time names,
     * prefer ENTROPY_PROFILE_ZONE_N. Uses RAII for automatic measurement.
     * 
     * @code
     * void processItem(const Item& item) {
     *     // Runtime zone name based on item
     *     ProfileZone zone(item.getName().c_str());
     *     
     *     // Work is automatically timed until zone exits scope
     *     item.process();
     * }
     * 
     * // Dynamic name formatting
     * void updateEntity(int id) {
     *     char buffer[64];
     *     snprintf(buffer, sizeof(buffer), "UpdateEntity_%d", id);
     *     ProfileZone zone(buffer);
     *     
     *     // Each entity gets a unique profiling zone
     * }
     * @endcode
     */
    class ProfileZone {
    public:
        #ifdef TRACY_ENABLE
        explicit ProfileZone(const char* name) {
            ZoneScoped;
            ZoneName(name, strlen(name));
        }
        #else
        explicit ProfileZone(const char* name) {
            (void)name;
        }
        #endif
    };

    /**
     * @brief Common Tracy colors for consistency
     * 
     * Predefined colors identify subsystems in the profiler timeline.
     * Use with ENTROPY_PROFILE_ZONE_C or ENTROPY_PROFILE_ZONE_NC.
     * 
     * @code
     * void renderFrame() {
     *     ENTROPY_PROFILE_ZONE_C(ProfileColors::Rendering);
     *     // Zone appears in red in the profiler
     * }
     * 
     * void updatePhysics() {
     *     ENTROPY_PROFILE_ZONE_NC("Physics Update", ProfileColors::Physics);
     *     // Green zone with custom name
     * }
     * @endcode
     */
    namespace ProfileColors {
        constexpr uint32_t Engine = 0x4444FF;      // Blue for engine code
        constexpr uint32_t Rendering = 0xFF4444;   // Red for rendering
        constexpr uint32_t Physics = 0x44FF44;     // Green for physics
        constexpr uint32_t Audio = 0xFFFF44;       // Yellow for audio
        constexpr uint32_t Networking = 0xFF44FF;  // Magenta for networking
        constexpr uint32_t IO = 0x44FFFF;          // Cyan for I/O operations
        constexpr uint32_t Memory = 0xFF8844;      // Orange for memory ops
        constexpr uint32_t Script = 0x8844FF;      // Purple for scripting
    }

} // namespace Debug
} // namespace Core
} // namespace EntropyEngine

