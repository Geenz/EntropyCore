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
#include "Core/EntropyService.h"
#include "TypeSystem/TypeID.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <unordered_set>
#include <type_traits>

namespace EntropyEngine {
    namespace Core {

        /**
         * @brief Registry and lifecycle orchestrator for EntropyService instances.
         *
         * Services are registered and looked up by static TypeID (RTTI-less). The registry
         * can load/start/stop/unload all registered services honoring their declared type
         * dependencies.
         */
        class EntropyServiceRegistry {
        public:
            EntropyServiceRegistry() = default;
            ~EntropyServiceRegistry() = default;

            // Non-copyable, movable
            EntropyServiceRegistry(const EntropyServiceRegistry&) = delete;
            EntropyServiceRegistry& operator=(const EntropyServiceRegistry&) = delete;

            // Registration API
            bool registerService(std::shared_ptr<EntropyService> service);
            // Preferred: register with static type to avoid dynamic RTTI lookups
            template<typename TService>
            bool registerService(std::shared_ptr<TService> service) {
                static_assert(std::is_base_of_v<EntropyService, TService>, "TService must derive from EntropyService");
                if (!service) return false;
                auto tid = TypeSystem::createTypeId<TService>();
                bool inserted = (_servicesByType.find(tid) == _servicesByType.end());
                _servicesByType[tid] = service;
                return inserted;
            }

            // Type-based lookup API
            std::shared_ptr<EntropyService> get(const TypeSystem::TypeID& tid) const;
            template<typename T>
            std::shared_ptr<T> get() const {
                auto base = get(TypeSystem::createTypeId<T>());
                // Safe to static_cast because map key is compile-time TypeID for T
                return std::static_pointer_cast<T>(base);
            }
            bool has(const TypeSystem::TypeID& tid) const noexcept;
            template<typename T>
            bool has() const noexcept { return has(TypeSystem::createTypeId<T>()); }

            size_t serviceCount() const noexcept { return _servicesByType.size(); }

            // Lifecycle control (throws std::runtime_error on dependency errors)
            void loadAll();
            void startAll();
            void stopAll();
            void unloadAll();

        private:
            // Returns topologically sorted type ids according to dependencies
            std::vector<TypeSystem::TypeID> topoOrder() const;

            std::unordered_map<TypeSystem::TypeID, std::shared_ptr<EntropyService>> _servicesByType; // static type -> service
        };

    } // namespace Core
} // namespace EntropyEngine
