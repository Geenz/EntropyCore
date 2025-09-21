/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file AcyclicNodeHandle.h
 * @brief EntropyObject-based safe handles for DAG nodes
 *
 * AcyclicNodeHandle<T> derives from EntropyObject and is stamped by
 * DirectedAcyclicGraph<T> with (owner + index + generation). This preserves
 * generation-based validation and enables uniform diagnostics and interop.
 */

#pragma once
#include <cstdint>
#include "../Core/EntropyObject.h"
#include "../TypeSystem/TypeID.h" // for classHash()

namespace EntropyEngine {
    namespace Core {
        namespace Graph {
            template<class T>
            class DirectedAcyclicGraph;

            // Optional tag retained for compatibility with any external code
            struct NodeTag {};

            template<class T>
            class AcyclicNodeHandle : public EntropyEngine::Core::EntropyObject {
                using GraphT = DirectedAcyclicGraph<T>;
                template<typename U>
                friend class DirectedAcyclicGraph;
            public:
                // Default: invalid (no stamped identity)
                AcyclicNodeHandle() = default;

                // Stamping constructor used by DirectedAcyclicGraph<T>
                AcyclicNodeHandle(GraphT* graph, uint32_t index, uint32_t generation) {
                    EntropyEngine::Core::HandleAccess::set(*this, graph, index, generation);
                }

                // Copy constructor: base default-constructed; copy stamp from other
                AcyclicNodeHandle(const AcyclicNodeHandle& other) noexcept {
                    if (other.hasHandle()) {
                        EntropyEngine::Core::HandleAccess::set(
                            *this,
                            const_cast<void*>(other.handleOwner()),
                            other.handleIndex(),
                            other.handleGeneration());
                    }
                }
                // Copy assignment: copy or clear stamp
                AcyclicNodeHandle& operator=(const AcyclicNodeHandle& other) noexcept {
                    if (this != &other) {
                        if (other.hasHandle()) {
                            EntropyEngine::Core::HandleAccess::set(
                                *this,
                                const_cast<void*>(other.handleOwner()),
                                other.handleIndex(),
                                other.handleGeneration());
                        } else {
                            EntropyEngine::Core::HandleAccess::clear(*this);
                        }
                    }
                    return *this;
                }
                // Move constructor: copy stamp (handles are lightweight)
                AcyclicNodeHandle(AcyclicNodeHandle&& other) noexcept {
                    if (other.hasHandle()) {
                        EntropyEngine::Core::HandleAccess::set(
                            *this,
                            const_cast<void*>(other.handleOwner()),
                            other.handleIndex(),
                            other.handleGeneration());
                    }
                }
                // Move assignment
                AcyclicNodeHandle& operator=(AcyclicNodeHandle&& other) noexcept {
                    if (this != &other) {
                        if (other.hasHandle()) {
                            EntropyEngine::Core::HandleAccess::set(
                                *this,
                                const_cast<void*>(other.handleOwner()),
                                other.handleIndex(),
                                other.handleGeneration());
                        } else {
                            EntropyEngine::Core::HandleAccess::clear(*this);
                        }
                    }
                    return *this;
                }

                // Diagnostics
                const char* className() const noexcept override { return "AcyclicNodeHandle"; }
                uint64_t classHash() const noexcept override {
                    using EntropyEngine::Core::TypeSystem::createTypeId;
                    static const uint64_t hash = static_cast<uint64_t>(createTypeId< AcyclicNodeHandle<T> >().id);
                    return hash;
                }
            };

            // Equality operators for tests and containers
            template<class T>
            inline bool operator==(const AcyclicNodeHandle<T>& a, const AcyclicNodeHandle<T>& b) noexcept {
                return a.handleOwner() == b.handleOwner() && a.handleId() == b.handleId();
            }
            template<class T>
            inline bool operator!=(const AcyclicNodeHandle<T>& a, const AcyclicNodeHandle<T>& b) noexcept {
                return !(a == b);
            }
        }
    }
}

