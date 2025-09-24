/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "Core/EntropyServiceRegistry.h"
#include <queue>
#include <unordered_map>

namespace EntropyEngine {
    namespace Core {

        bool EntropyServiceRegistry::registerService(std::shared_ptr<EntropyService> service) {
            if (!service) return false;
            auto tid = service->typeId();
            bool inserted = (_servicesByType.find(tid) == _servicesByType.end());
            _servicesByType[tid] = std::move(service);
            return inserted;
        }

        std::shared_ptr<EntropyService> EntropyServiceRegistry::get(const TypeSystem::TypeID& tid) const {
            auto it = _servicesByType.find(tid);
            if (it == _servicesByType.end()) return nullptr;
            return it->second;
        }

        bool EntropyServiceRegistry::has(const TypeSystem::TypeID& tid) const noexcept {
            return _servicesByType.find(tid) != _servicesByType.end();
        }

        void EntropyServiceRegistry::loadAll() {
            auto order = topoOrder();
            for (const auto& tid : order) {
                auto& svc = _servicesByType.at(tid);
                svc->setState(ServiceState::Loaded);
                svc->load();
            }
        }

        void EntropyServiceRegistry::startAll() {
            auto order = topoOrder();
            for (const auto& tid : order) {
                auto& svc = _servicesByType.at(tid);
                svc->setState(ServiceState::Started);
                svc->start();
            }
        }

        void EntropyServiceRegistry::stopAll() {
            auto order = topoOrder();
            // stop in reverse order
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                auto& svc = _servicesByType.at(*it);
                svc->stop();
                svc->setState(ServiceState::Stopped);
            }
        }

        void EntropyServiceRegistry::unloadAll() {
            auto order = topoOrder();
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                auto& svc = _servicesByType.at(*it);
                svc->unload();
                svc->setState(ServiceState::Unloaded);
            }
        }

        std::vector<TypeSystem::TypeID> EntropyServiceRegistry::topoOrder() const {
            // Kahn's algorithm on TypeIDs
            // Build adjacency: dep -> svc
            std::unordered_map<TypeSystem::TypeID, size_t> indegree;
            std::unordered_map<TypeSystem::TypeID, std::vector<TypeSystem::TypeID>> adj;

            // Initialize vertices
            for (const auto& [tid, _] : _servicesByType) {
                indegree[tid] = 0;
            }

            // Add edges and indegrees using type-based dependencies
            for (const auto& [tid, svc] : _servicesByType) {
                for (const auto& depTid : svc->dependsOnTypes()) {
                    if (!has(depTid)) {
                        // Diagnostic message uses metadata strings, not for lookup
                        throw std::runtime_error(std::string("Missing dependency required by service '") + svc->id() + "'");
                    }
                    adj[depTid].push_back(tid);
                    indegree[tid] += 1;
                }
            }

            std::queue<TypeSystem::TypeID> q;
            for (const auto& [tid, deg] : indegree) {
                if (deg == 0) q.push(tid);
            }

            std::vector<TypeSystem::TypeID> order;
            order.reserve(_servicesByType.size());
            while (!q.empty()) {
                auto u = q.front(); q.pop();
                order.push_back(u);
                auto it = adj.find(u);
                if (it != adj.end()) {
                    for (const auto& v : it->second) {
                        auto& d = indegree[v];
                        if (d == 0) continue; // defensive
                        d -= 1;
                        if (d == 0) q.push(v);
                    }
                }
            }

            if (order.size() != _servicesByType.size()) {
                throw std::runtime_error("Service dependency cycle detected");
            }

            return order;
        }

    } // namespace Core
} // namespace EntropyEngine
