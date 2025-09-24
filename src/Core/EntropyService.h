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
#include "TypeSystem/TypeID.h"

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

            // Identity (metadata only; not used for lookups)
            virtual const char* id() const = 0;    // stable unique id, e.g. "com.entropy.core.work"
            virtual const char* name() const = 0;  // human readable

            // Static type identity for RTTI-less registration and lookup
            virtual TypeSystem::TypeID typeId() const = 0;

            // Optional semantic version for compatibility checks
            virtual const char* version() const { return "0.1.0"; }

            // RTTI-less dependency declaration by static types. Preferred and used for ordering.
            virtual std::vector<TypeSystem::TypeID> dependsOnTypes() const { return {}; }

            // (Legacy metadata) String-based dependencies retained for diagnostics only; ignored by orchestrator.
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
