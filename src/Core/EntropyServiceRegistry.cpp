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

namespace EntropyEngine
{
namespace Core
{

EntropyServiceRegistry::~EntropyServiceRegistry() {
    // Clear all handle stamps before destruction
    for (auto& [tid, slot] : _slots) {
        if (slot.service) {
            HandleSlotOps::clear(*slot.service);
        }
    }
}

bool EntropyServiceRegistry::registerService(RefObject<EntropyService> service) {
    if (!service) return false;

    auto tid = service->typeId();
    auto it = _slots.find(tid);

    if (it != _slots.end()) {
        // Service already registered - clear old stamp and update
        if (it->second.service) {
            HandleSlotOps::release(*it->second.service, it->second.generation);
        }
        it->second.service = std::move(service);
        // Stamp with new generation
        HandleSlotOps::stamp(*it->second.service, this, it->second.slotIndex, it->second.generation);
        return false;  // Not a new insertion
    }

    // New service - create slot
    ServiceSlot slot;
    slot.service = std::move(service);
    slot.slotIndex = _nextSlotIndex++;

    // Stamp service with handle identity
    HandleSlotOps::stamp(*slot.service, this, slot.slotIndex, slot.generation);

    _slots[tid] = std::move(slot);
    return true;
}

bool EntropyServiceRegistry::unregisterService(const TypeSystem::TypeID& tid) {
    auto it = _slots.find(tid);
    if (it == _slots.end()) return false;

    ServiceSlot& slot = it->second;
    if (slot.service) {
        // Clear handle and increment generation to invalidate WeakRefs
        HandleSlotOps::release(*slot.service, slot.generation);
    }

    _slots.erase(it);
    return true;
}

RefObject<EntropyService> EntropyServiceRegistry::get(const TypeSystem::TypeID& tid) const {
    auto slot = getSlot(tid);
    if (!slot || !slot->service) return {};
    // Return a new RefObject with an additional retain
    return RefObject<EntropyService>(retain, slot->service.get());
}

bool EntropyServiceRegistry::has(const TypeSystem::TypeID& tid) const noexcept {
    return _slots.find(tid) != _slots.end();
}

bool EntropyServiceRegistry::isValid(const EntropyService* service) const noexcept {
    if (!service || !service->hasHandle()) return false;
    if (service->handleOwner() != this) return false;

    // Find the slot by iterating (services are keyed by TypeID, not slot index)
    for (const auto& [tid, slot] : _slots) {
        if (slot.service.get() == service) {
            return slot.generation.current() == service->handleGeneration();
        }
    }
    return false;
}

const EntropyServiceRegistry::ServiceSlot* EntropyServiceRegistry::getSlot(const TypeSystem::TypeID& tid) const {
    auto it = _slots.find(tid);
    return it != _slots.end() ? &it->second : nullptr;
}

EntropyServiceRegistry::ServiceSlot* EntropyServiceRegistry::getSlot(const TypeSystem::TypeID& tid) {
    auto it = _slots.find(tid);
    return it != _slots.end() ? &it->second : nullptr;
}

void EntropyServiceRegistry::loadAll() {
    auto order = topoOrder();
    for (const auto& tid : order) {
        auto& slot = _slots.at(tid);
        slot.service->setState(ServiceState::Loaded);
        slot.service->load();
    }
}

void EntropyServiceRegistry::startAll() {
    auto order = topoOrder();
    for (const auto& tid : order) {
        auto& slot = _slots.at(tid);
        slot.service->setState(ServiceState::Started);
        slot.service->start();
    }
}

void EntropyServiceRegistry::stopAll() {
    auto order = topoOrder();
    // stop in reverse order
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        auto& slot = _slots.at(*it);
        slot.service->stop();
        slot.service->setState(ServiceState::Stopped);
    }
}

void EntropyServiceRegistry::unloadAll() {
    auto order = topoOrder();
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        auto& slot = _slots.at(*it);
        slot.service->unload();
        slot.service->setState(ServiceState::Unloaded);
    }
}

std::vector<TypeSystem::TypeID> EntropyServiceRegistry::topoOrder() const {
    // Kahn's algorithm on TypeIDs
    // Build adjacency: dep -> svc
    std::unordered_map<TypeSystem::TypeID, size_t> indegree;
    std::unordered_map<TypeSystem::TypeID, std::vector<TypeSystem::TypeID>> adj;

    // Initialize vertices
    for (const auto& [tid, _] : _slots) {
        indegree[tid] = 0;
    }

    // Add edges and indegrees using type-based dependencies
    for (const auto& [tid, slot] : _slots) {
        for (const auto& depTid : slot.service->dependsOnTypes()) {
            if (!has(depTid)) {
                // Diagnostic message uses metadata strings, not for lookup
                throw std::runtime_error(std::string("Missing dependency required by service '") + slot.service->id() +
                                         "'");
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
    order.reserve(_slots.size());
    while (!q.empty()) {
        auto u = q.front();
        q.pop();
        order.push_back(u);
        auto it = adj.find(u);
        if (it != adj.end()) {
            for (const auto& v : it->second) {
                auto& d = indegree[v];
                if (d == 0) continue;  // defensive
                d -= 1;
                if (d == 0) q.push(v);
            }
        }
    }

    if (order.size() != _slots.size()) {
        throw std::runtime_error("Service dependency cycle detected");
    }

    return order;
}

}  // namespace Core
}  // namespace EntropyEngine
