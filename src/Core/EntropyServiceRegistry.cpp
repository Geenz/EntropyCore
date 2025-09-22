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

namespace EntropyEngine {
    namespace Core {

        bool EntropyServiceRegistry::registerService(std::shared_ptr<EntropyService> service) {
            if (!service) return false;
            auto key = std::string(service->id());
            auto it = _services.find(key);
            bool inserted = (it == _services.end());
            _services[key] = std::move(service);
            return inserted;
        }

        std::shared_ptr<EntropyService> EntropyServiceRegistry::get(const std::string& id) const {
            auto it = _services.find(id);
            if (it == _services.end()) return nullptr;
            return it->second;
        }

        bool EntropyServiceRegistry::has(const std::string& id) const noexcept {
            return _services.find(id) != _services.end();
        }

        void EntropyServiceRegistry::loadAll() {
            auto order = topoOrder();
            for (const auto& id : order) {
                auto& svc = _services.at(id);
                svc->setState(ServiceState::Loaded);
                svc->load();
            }
        }

        void EntropyServiceRegistry::startAll() {
            auto order = topoOrder();
            for (const auto& id : order) {
                auto& svc = _services.at(id);
                svc->setState(ServiceState::Started);
                svc->start();
            }
        }

        void EntropyServiceRegistry::stopAll() {
            auto order = topoOrder();
            // stop in reverse order
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                auto& svc = _services.at(*it);
                svc->stop();
                svc->setState(ServiceState::Stopped);
            }
        }

        void EntropyServiceRegistry::unloadAll() {
            auto order = topoOrder();
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                auto& svc = _services.at(*it);
                svc->unload();
                svc->setState(ServiceState::Unloaded);
            }
        }

        std::vector<std::string> EntropyServiceRegistry::topoOrder() const {
            // Kahn's algorithm
            // Build adjacency: dep -> svc
            std::unordered_map<std::string, size_t> indegree;
            std::unordered_map<std::string, std::vector<std::string>> adj;

            // Initialize vertices
            for (const auto& [id, _] : _services) {
                indegree[id] = 0;
            }

            // Add edges and indegrees
            for (const auto& [id, svc] : _services) {
                for (const auto& dep : svc->dependsOn()) {
                    if (!has(dep)) {
                        throw std::runtime_error("Missing dependency '" + dep + "' required by service '" + id + "'");
                    }
                    adj[dep].push_back(id);
                    indegree[id] += 1;
                }
            }

            std::queue<std::string> q;
            for (const auto& [id, deg] : indegree) {
                if (deg == 0) q.push(id);
            }

            std::vector<std::string> order;
            order.reserve(_services.size());
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

            if (order.size() != _services.size()) {
                throw std::runtime_error("Service dependency cycle detected");
            }

            return order;
        }

    } // namespace Core
} // namespace EntropyEngine
