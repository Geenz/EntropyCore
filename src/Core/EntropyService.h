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
#include <string>
#include <vector>
#include <atomic>

namespace EntropyEngine {
    namespace Core {

        class EntropyServiceRegistry; // forward declaration

        /**
         * @brief Lifecycle states for an EntropyService instance.
         */
        enum class ServiceState {
            Registered,
            Loaded,
            Started,
            Stopped,
            Unloaded
        };

        /**
         * @brief Base interface for pluggable services within Entropy.
         *
         * Services encapsulate optional subsystems (e.g., Work execution, Scene, Renderer)
         * and participate in the application lifecycle via load/start/stop/unload callbacks.
         *
         * Implementations should be lightweight to construct; heavy initialization should
         * happen in load()/start(). All lifecycle methods are expected to be called on the
         * main thread by the orchestrator.
         */
        class EntropyService {
        public:
            virtual ~EntropyService() = default;

            // Identity
            virtual const char* id() const = 0;    // stable unique id, e.g. "com.entropy.core.work"
            virtual const char* name() const = 0;  // human readable

            // Optional semantic version for compatibility checks
            virtual const char* version() const { return "0.1.0"; }

            // Declare service dependencies by id(). These must be available and started
            // before this service's start() is invoked.
            virtual std::vector<std::string> dependsOn() const { return {}; }

            // Lifecycle hooks (main thread unless documented otherwise)
            virtual void load() {}
            virtual void start() {}
            virtual void stop() {}
            virtual void unload() {}

            // Observability
            ServiceState state() const noexcept { return _state.load(std::memory_order_acquire); }

        protected:
            // For orchestration: allow registry/application to transition state
            void setState(ServiceState s) noexcept { _state.store(s, std::memory_order_release); }

        private:
            friend class EntropyServiceRegistry; // allow registry to drive state transitions
            std::atomic<ServiceState> _state{ServiceState::Registered};
        };

    } // namespace Core
} // namespace EntropyEngine
