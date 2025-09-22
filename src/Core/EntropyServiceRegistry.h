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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <unordered_set>

namespace EntropyEngine {
    namespace Core {

        /**
         * @brief Registry and lifecycle orchestrator for EntropyService instances.
         *
         * Services are registered by unique id(). The registry can load/start/stop/unload
         * all registered services honoring their declared dependencies.
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
            std::shared_ptr<EntropyService> get(const std::string& id) const;
            bool has(const std::string& id) const noexcept;
            size_t serviceCount() const noexcept { return _services.size(); }

            // Lifecycle control (throws std::runtime_error on dependency errors)
            void loadAll();
            void startAll();
            void stopAll();
            void unloadAll();

        private:
            // Returns topologically sorted ids according to dependencies
            std::vector<std::string> topoOrder() const;

            std::unordered_map<std::string, std::shared_ptr<EntropyService>> _services; // id -> service
        };

    } // namespace Core
} // namespace EntropyEngine
