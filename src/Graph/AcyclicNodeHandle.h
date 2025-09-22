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

            /**
             * @class AcyclicNodeHandle
             * @brief Lightweight, stamped handle to a node in a DirectedAcyclicGraph<T>
             *
             * AcyclicNodeHandle<T> is an EntropyObject that carries an owner+index+generation
             * identity stamped by DirectedAcyclicGraph<T>. The generation prevents stale-handle
             * reuse after node removal. Handles are cheap to copy and compare.
             *
             * @code
             * using namespace EntropyEngine::Core::Graph;
             * DirectedAcyclicGraph<int> dag;
             * auto a = dag.addNode(1);
             * auto b = dag.addNode(2);
             * dag.addEdge(a, b);           // b depends on a
             *
             * // Validate and access data
             * if (dag.isHandleValid(a)) {
             *     int* data = dag.getNodeData(a);
             *     if (data) { *data = 42; }
             * }
             *
             * // Equality compares owner and stamped id (index:generation)
             * bool same = (a == a);
             * bool different = (a != b);
             * @endcode
             */
            template<class T>
            class AcyclicNodeHandle : public EntropyEngine::Core::EntropyObject {
                using GraphT = DirectedAcyclicGraph<T>;
                template<typename U>
                friend class DirectedAcyclicGraph;
            public:
                /** @brief Default-constructed handle with no identity (invalid) */
                AcyclicNodeHandle() = default;

                /**
                 * @brief Internal constructor used by DirectedAcyclicGraph to stamp identity
                 * @param graph Owning graph that stamps the handle
                 * @param index Slot index within the graph
                 * @param generation Generation counter for stale-handle detection
                 */
                AcyclicNodeHandle(GraphT* graph, uint32_t index, uint32_t generation) {
                    EntropyEngine::Core::HandleAccess::set(*this, graph, index, generation);
                }

                /**
                 * @brief Copies the stamped identity from another handle (if present)
                 */
                AcyclicNodeHandle(const AcyclicNodeHandle& other) noexcept {
                    if (other.hasHandle()) {
                        EntropyEngine::Core::HandleAccess::set(
                            *this,
                            const_cast<void*>(other.handleOwner()),
                            other.handleIndex(),
                            other.handleGeneration());
                    }
                }
                /**
                 * @brief Copies or clears the stamped identity depending on source validity
                 */
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
                /**
                 * @brief Moves by copying the stamped identity (handles are lightweight)
                 */
                AcyclicNodeHandle(AcyclicNodeHandle&& other) noexcept {
                    if (other.hasHandle()) {
                        EntropyEngine::Core::HandleAccess::set(
                            *this,
                            const_cast<void*>(other.handleOwner()),
                            other.handleIndex(),
                            other.handleGeneration());
                    }
                }
                /**
                 * @brief Move-assigns by copying or clearing identity based on source validity
                 */
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

                /** @brief Runtime type name for diagnostics */
                const char* className() const noexcept override { return "AcyclicNodeHandle"; }
                /** @brief Stable type hash for cross-language identification */
                uint64_t classHash() const noexcept override {
                    using EntropyEngine::Core::TypeSystem::createTypeId;
                    static const uint64_t hash = static_cast<uint64_t>(createTypeId< AcyclicNodeHandle<T> >().id);
                    return hash;
                }
            };

            /**
             * @brief Equality compares owning graph and packed index:generation id
             * @return true if both handles refer to the same stamped node
             */
            template<class T>
            inline bool operator==(const AcyclicNodeHandle<T>& a, const AcyclicNodeHandle<T>& b) noexcept {
                return a.handleOwner() == b.handleOwner() && a.handleId() == b.handleId();
            }
            /**
             * @brief Inequality is the negation of equality
             */
            template<class T>
            inline bool operator!=(const AcyclicNodeHandle<T>& a, const AcyclicNodeHandle<T>& b) noexcept {
                return !(a == b);
            }
        }
    }
}

