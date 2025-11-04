/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file Debug.h
 * @brief Master header for all debug utilities
 * 
 * This file consolidates all debug tools: object tracking, profiling, assertions,
 * and logging. Include this single header to access the complete debugging toolkit.
 */

#pragma once

#include "INamed.h"
#include "DebugUtilities.h"
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <vector>
#include <format>

namespace EntropyEngine {
namespace Core {
namespace Debug {

    /**
     * @brief Central command center for all debugging operations
     * 
     * Call initialize() at startup to get profiling, logging, and object
     * tracking running. Most features compile to nothing in release builds.
     */
    class DebugSystem {
    public:
        /**
         * @brief Fire up all debug systems - call this once at startup
         * 
         * Initializes Tracy profiler, logging, object tracking, and build
         * configuration reporting. Safe to call multiple times.
         */
        static void initialize() {
            ENTROPY_LOG_INFO_CAT("Debug", "Debug system initialized");

            #ifdef EntropyDebug
                ENTROPY_LOG_INFO_CAT("Debug", "Debug build - assertions enabled");
            #else
                ENTROPY_LOG_INFO_CAT("Debug", "Release build - assertions disabled");
            #endif
        }
        
        /**
         * @brief Clean shutdown of debug systems
         * 
         * Flushes pending data and shuts down profiling. Optional but recommended.
         */
        static void shutdown() {
            ENTROPY_LOG_INFO_CAT("Debug", "Debug system shutdown");
        }
    };

    /**
     * @brief Registry for runtime object tracking and discovery
     * 
     * Tracks INamed objects with their types, names, and creation times.
     * Thread-safe. Useful for debugging lifecycle issues and leak detection.
     * 
     * @code
     * // Find all WorkGraphs
     * auto graphs = DebugRegistry::getInstance().findByType("WorkGraph");
     * LOG_INFO("Found {} active WorkGraphs", graphs.size());
     * 
     * // Find a specific object
     * auto results = DebugRegistry::getInstance().findByName("MainMenuUI");
     * if (!results.empty()) {
     *     LOG_INFO("MainMenuUI is at {}", results[0]);
     * }
     * @endcode
     */
    class DebugRegistry {
    private:
        struct Entry {
            const INamed* object;                           ///< Pointer to the tracked object
            std::string typeName;                           ///< Type name for categorization
            std::chrono::system_clock::time_point creationTime;  ///< When it was registered
        };
        
        mutable std::shared_mutex _mutex;
        std::unordered_map<const INamed*, Entry> _objects;
        
        static DebugRegistry* s_instance;
        static std::mutex s_instanceMutex;
        
    public:
        static DebugRegistry& getInstance() {
            std::lock_guard<std::mutex> lock(s_instanceMutex);
            if (!s_instance) {
                s_instance = new DebugRegistry();
            }
            return *s_instance;
        }
        
        /**
         * @brief Add an object to the registry
         * 
         * Registry holds non-owning pointers - unregister before destruction.
         * 
         * @param object The object to track (must outlive the registration)
         * @param typeName Human-readable type name for grouping
         */
        void registerObject(const INamed* object, std::string_view typeName) {
            std::unique_lock<std::shared_mutex> lock(_mutex);
            _objects[object] = Entry{
                object,
                std::string(typeName),
                std::chrono::system_clock::now()
            };
            
            auto msg = std::format("Registered {} '{}' at {}",
                                  typeName, object->getName(), static_cast<const void*>(object));
            ENTROPY_LOG_TRACE_CAT("DebugRegistry", msg);
        }
        
        /**
         * @brief Remove an object from tracking
         * 
         * Must be called before object destruction to avoid dangling pointers.
         * 
         * @param object The object to stop tracking
         */
        void unregisterObject(const INamed* object) {
            std::unique_lock<std::shared_mutex> lock(_mutex);
            auto it = _objects.find(object);
            if (it != _objects.end()) {
                auto msg = std::format("Unregistered {} '{}' at {}",
                                      it->second.typeName, object->getName(), static_cast<const void*>(object));
                ENTROPY_LOG_TRACE_CAT("DebugRegistry", msg);
                _objects.erase(it);
            }
        }
        
        /**
         * @brief Search for all objects with a specific name
         * 
         * Names can be non-unique. Returns all exact matches.
         * 
         * @param name The name to search for
         * @return Vector of matching object pointers
         */
        [[nodiscard]] std::vector<const INamed*> findByName(std::string_view name) const {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            std::vector<const INamed*> results;
            
            for (const auto& [object, entry] : _objects) {
                if (object->getName() == name) {
                    results.push_back(object);
                }
            }
            
            return results;
        }
        
        /**
         * @brief Find all objects of a specific type
         * 
         * Useful for counting active instances or type-specific debugging.
         * 
         * @param typeName The type name used during registration
         * @return Vector of all objects of this type
         */
        [[nodiscard]] std::vector<const INamed*> findByType(std::string_view typeName) const {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            std::vector<const INamed*> results;
            
            for (const auto& [object, entry] : _objects) {
                if (entry.typeName == typeName) {
                    results.push_back(object);
                }
            }
            
            return results;
        }
        
        /**
         * @brief Dump the entire registry to the log
         * 
         * Outputs all registered objects grouped by type. Can be verbose!
         * 
         * Output format:
         * ```
         * === Registered Debug Objects (42) ===
         * WorkGraph (3 instances):
         *   - 'MainGraph' at 0x7fff12345678
         *   - 'UIGraph' at 0x7fff87654321
         *   - 'AudioGraph' at 0x7fff11111111
         * EventBus (15 instances):
         *   ...
         * ```
         */
        void logAllObjects() const {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            
            auto headerMsg = std::format("=== Registered Debug Objects ({}) ===", _objects.size());
            ENTROPY_LOG_INFO_CAT("DebugRegistry", headerMsg);
            
            // Group by type
            std::unordered_map<std::string, std::vector<const INamed*>> byType;
            for (const auto& [object, entry] : _objects) {
                byType[entry.typeName].push_back(object);
            }
            
            // Log grouped by type
            for (const auto& [typeName, objects] : byType) {
                auto typeMsg = std::format("{} ({} instances):", typeName, objects.size());
                ENTROPY_LOG_INFO_CAT("DebugRegistry", typeMsg);
                for (const auto* obj : objects) {
                    auto objMsg = std::format("  - '{}' at {}", 
                                         obj->getName(), static_cast<const void*>(obj));
                    ENTROPY_LOG_INFO_CAT("DebugRegistry", objMsg);
                }
            }
        }
    };

    /**
     * @brief RAII wrapper for automatic debug registration
     * 
     * Inherit from this to get automatic registration/unregistration.
     * No more forgetting to unregister!
     * 
     * @tparam T The base class that implements INamed
     * 
     * @code
     * // Instead of:
     * class MySystem : public INamed {
     *     MySystem() : INamed("MySystem") {
     *         DebugRegistry::getInstance().registerObject(this, "MySystem");
     *     }
     *     ~MySystem() {
     *         DebugRegistry::getInstance().unregisterObject(this);
     *     }
     * };
     * 
     * // Just do:
     * class MySystem : public AutoDebugRegistered<INamed> {
     *     MySystem() : AutoDebugRegistered("MySystem", "MySystem") {}
     * };
     * @endcode
     */
    template<typename T>
    class AutoDebugRegistered : public T {
    public:
        template<typename... Args>
        explicit AutoDebugRegistered(std::string_view typeName, Args&&... args) 
            : T(std::forward<Args>(args)...) {
            DebugRegistry::getInstance().registerObject(this, typeName);
        }
        
        ~AutoDebugRegistered() {
            DebugRegistry::getInstance().unregisterObject(this);
        }
    };

} // namespace Debug
} // namespace Core
} // namespace EntropyEngine

// Initialize static members
inline EntropyEngine::Core::Debug::DebugRegistry* EntropyEngine::Core::Debug::DebugRegistry::s_instance = nullptr;
inline std::mutex EntropyEngine::Core::Debug::DebugRegistry::s_instanceMutex;

