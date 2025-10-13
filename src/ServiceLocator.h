/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#pragma once

#include "CoreCommon.h"
#include "TypeSystem/Reflection.h"
#include <memory>
#include <shared_mutex>
#include <mutex>
#include <unordered_map>
#include <stdexcept>

namespace EntropyEngine {
    namespace Core {

        /**
         * @brief A thread-safe registry for global services that need to be accessible throughout the engine.
         * 
         * ServiceLocator provides a centralized registry for locating services such as
         * WindowService or WorkService. It serves as a central hub that enables different
         * parts of the engine to find and communicate with each other without creating
         * direct dependencies between components.
         * 
         * The ServiceLocator is a singleton that maintains a type-safe registry of services.
         * Services are stored as shared_ptr objects, so multiple systems can safely hold
         * references to the same service. Thread-safety is built in, so you can register
         * and access services from any thread without worrying about race conditions.
         * 
         * This pattern is especially useful for:
         * - Core engine services that many systems need (rendering, audio, input)
         * - Breaking circular dependencies between systems
         * - Late initialization of services (register when ready, access when needed)
         * - Clean shutdown (services can be removed in proper order)
         * 
         * @code
         * // Register a service during initialization
         * auto windowService = std::make_shared<WindowService>();
         * ServiceLocator::instance().registerService<WindowService>(windowService);
         * 
         * // Access the service from anywhere in the engine
         * auto service = ServiceLocator::instance().getService<WindowService>();
         * if (service) {
         *     service->createWindow("My Game", 1920, 1080);
         * }
         * 
         * // Clean up during shutdown
         * ServiceLocator::instance().removeService<WindowService>();
         * @endcode
         */
        class ServiceLocator {
        public:
            /**
             * @brief Gets the singleton ServiceLocator instance
             * 
             * Thread-safe using Meyer's singleton pattern. First call creates
             * the instance, subsequent calls return the same instance.
             * 
             * @return Reference to the singleton ServiceLocator instance
             * 
             * @code
             * // Access from anywhere in your code
             * auto& locator = ServiceLocator::instance();
             * locator.registerService<MyService>(myServiceInstance);
             * @endcode
             */
            static ServiceLocator& instance() {
                static ServiceLocator _instance;
                return _instance;
            }

            /**
             * @brief Registers a service instance by type
             * 
             * Stores service in global registry. Replaces existing service of
             * same type. Thread-safe with exclusive locking.
             * 
             * @tparam T The service type - use interface type, not implementation
             * @param service Shared pointer to the service instance
             * 
             * @code
             * // Register a concrete implementation via its interface
             * auto audioService = std::make_shared<OpenALAudioService>();
             * ServiceLocator::instance().registerService<IAudioService>(audioService);
             * 
             * // The service is now available to the entire engine
             * @endcode
             */
            template<typename T>
            void registerService(std::shared_ptr<T> service) {
                std::unique_lock<std::shared_mutex> lock(_mutex);
                _services[TypeSystem::createTypeId<T>()] = service;
            }

            /**
             * @brief Retrieves a registered service by type
             * 
             * Returns nullptr if not found, allowing graceful handling of missing
             * services. Thread-safe with shared locking for concurrent reads.
             * 
             * @tparam T The service type to retrieve
             * @return Shared pointer to the service, or nullptr if not found
             * 
             * @code
             * // Safe service access with null check
             * auto audioService = ServiceLocator::instance().getService<IAudioService>();
             * if (audioService) {
             *     audioService->playSound("explosion.wav");
             * } else {
             *     // Audio is disabled or not initialized yet
             *     std::cout << "Audio service not available" << std::endl;
             * }
             * @endcode
             */
            template<typename T>
            std::shared_ptr<T> getService() const {
                std::shared_lock<std::shared_mutex> lock(_mutex);
                auto it = _services.find(TypeSystem::createTypeId<T>());
                if (it == _services.end()) {
                    return nullptr;
                }
                return std::static_pointer_cast<T>(it->second);
            }

            /**
             * @brief Checks if a service of the given type is registered
             * 
             * Useful for checking optional services or conditional logic based
             * on available services.
             * 
             * @tparam T The service type to check for
             * @return true if service is registered, false otherwise
             * 
             * @code
             * // Conditional feature based on service availability
             * if (ServiceLocator::instance().hasService<IDebugService>()) {
             *     // Enable debug rendering and profiling
             *     enableDebugFeatures();
             * }
             * 
             * // Validation during startup
             * assert(ServiceLocator::instance().hasService<IRendererService>() && 
             *        "Renderer service must be initialized before starting main loop");
             * @endcode
             */
            template<typename T>
            bool hasService() const {
                std::shared_lock<std::shared_mutex> lock(_mutex);
                return _services.find(TypeSystem::createTypeId<T>()) != _services.end();
            }

            /**
             * @brief Removes a service from the registry
             * 
             * Unregisters service but doesn't destroy it immediately - shared_ptr
             * keeps it alive while references exist. Used during shutdown.
             * 
             * IMPORTANT: Uses shared lock for lookup, then upgrades to unique lock
             * for removal. Service is extracted under lock but destroyed outside
             * to prevent deadlocks if destructor calls removeService().
             * 
             * @tparam T The service type to remove
             * 
             * @code
             * // Shutdown sequence - remove services in reverse dependency order
             * ServiceLocator::instance().removeService<IUIService>();      // UI depends on rendering
             * ServiceLocator::instance().removeService<IRendererService>(); // Renderer depends on window
             * ServiceLocator::instance().removeService<IWindowService>();   // Window is foundational
             * @endcode
             */
            template<typename T>
            void removeService() {
                auto typeId = TypeSystem::createTypeId<T>();
                std::shared_ptr<void> serviceToDestroy;

                // First check if service exists with shared lock
                {
                    std::shared_lock<std::shared_mutex> readLock(_mutex);
                    auto it = _services.find(typeId);
                    if (it == _services.end()) {
                        return; // Service not found, nothing to do
                    }
                }

                // Service exists, now take exclusive lock to remove it
                {
                    std::unique_lock<std::shared_mutex> writeLock(_mutex);
                    auto it = _services.find(typeId);
                    if (it != _services.end()) {
                        // Extract the service but keep it alive via shared_ptr
                        serviceToDestroy = std::move(it->second);
                        _services.erase(it);
                    }
                }
                // Lock is released here - service destruction happens outside the lock
                // This prevents deadlocks if the destructor calls removeService() for other services
            }

        private:
            /**
             * @brief Private constructor to enforce singleton pattern
             */
            ServiceLocator() = default;
            
            /**
             * @brief Private destructor for singleton cleanup
             */
            ~ServiceLocator() = default;
            
            /**
             * @brief Deleted copy constructor to prevent copying
             */
            ServiceLocator(const ServiceLocator&) = delete;
            
            /**
             * @brief Deleted assignment operator to prevent copying
             */
            ServiceLocator& operator=(const ServiceLocator&) = delete;

            /**
             * @brief Reader-writer mutex for thread-safe access
             * 
             * Allows concurrent reads, exclusive writes. Mutable for const methods.
             */
            mutable std::shared_mutex _mutex;
            
            /**
             * @brief Service registry mapping type IDs to instances
             * 
             * Type-safe using TypeID keys. Values use type erasure with shared_ptr<void>.
             */
            std::unordered_map<TypeSystem::TypeID, std::shared_ptr<void>> _services;
        };

    } // namespace Core
} // namespace EntropyEngine