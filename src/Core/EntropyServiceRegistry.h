/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Core/EntropyService.h"
#include "Core/HandleSlot.h"
#include "Core/RefObject.h"
#include "CoreCommon.h"
#include "TypeSystem/TypeID.h"

namespace EntropyEngine
{
namespace Core
{

/**
 * @brief Registry and lifecycle orchestrator for EntropyService instances.
 *
 * Services are registered and looked up by static TypeID (RTTI-less). The registry
 * can load/start/stop/unload all registered services honoring their declared type
 * dependencies.
 *
 * Services are stamped with handle identity (owner/index/generation) on registration,
 * enabling generation-based validation and WeakRef support for safe cross-subsystem
 * references.
 */
class EntropyServiceRegistry
{
public:
    EntropyServiceRegistry() = default;
    ~EntropyServiceRegistry();

    // Non-copyable, non-movable (services hold back-references)
    EntropyServiceRegistry(const EntropyServiceRegistry&) = delete;
    EntropyServiceRegistry& operator=(const EntropyServiceRegistry&) = delete;
    EntropyServiceRegistry(EntropyServiceRegistry&&) = delete;
    EntropyServiceRegistry& operator=(EntropyServiceRegistry&&) = delete;

    // Registration API
    bool registerService(std::shared_ptr<EntropyService> service);
    // Preferred: register with static type to avoid dynamic RTTI lookups
    template <typename TService>
    bool registerService(std::shared_ptr<TService> service) {
        static_assert(std::is_base_of_v<EntropyService, TService>, "TService must derive from EntropyService");
        // Delegate to non-template version for stamping
        return registerService(std::static_pointer_cast<EntropyService>(service));
    }

    /**
     * @brief Unregister a service by type
     *
     * Clears the service's handle stamp and removes it from the registry.
     * The slot's generation is incremented to invalidate stale WeakRefs.
     *
     * @return true if service was found and unregistered
     */
    bool unregisterService(const TypeSystem::TypeID& tid);
    template <typename T>
    bool unregisterService() {
        return unregisterService(TypeSystem::createTypeId<T>());
    }

    // Type-based lookup API (returns shared_ptr for compatibility)
    std::shared_ptr<EntropyService> get(const TypeSystem::TypeID& tid) const;
    template <typename T>
    std::shared_ptr<T> get() const {
        auto base = get(TypeSystem::createTypeId<T>());
        return std::static_pointer_cast<T>(base);
    }

    /**
     * @brief Get a RefObject reference to a service
     *
     * Returns a RefObject that can be used with WeakRef for safe non-owning
     * references. The service is stamped with generation for validation.
     */
    template <typename T>
    RefObject<T> getRef() const {
        auto slot = getSlot(TypeSystem::createTypeId<T>());
        if (!slot || !slot->service) return {};
        // Create RefObject with retain
        return RefObject<T>(retain, static_cast<T*>(slot->service.get()));
    }

    bool has(const TypeSystem::TypeID& tid) const noexcept;
    template <typename T>
    bool has() const noexcept {
        return has(TypeSystem::createTypeId<T>());
    }

    /**
     * @brief Validate a service is still registered with matching generation
     */
    bool isValid(const EntropyService* service) const noexcept;

    size_t serviceCount() const noexcept {
        return _slots.size();
    }

    // Lifecycle control (throws std::runtime_error on dependency errors)
    void loadAll();
    void startAll();
    void stopAll();
    void unloadAll();

private:
    /**
     * @brief Internal slot for service storage with generation tracking
     */
    struct ServiceSlot
    {
        std::shared_ptr<EntropyService> service;  ///< The service instance
        SlotGeneration generation;                ///< Handle validation counter
        uint32_t slotIndex = 0;                   ///< Index used for handle stamping
    };

    // Returns topologically sorted type ids according to dependencies
    std::vector<TypeSystem::TypeID> topoOrder() const;

    // Get slot by type (internal)
    const ServiceSlot* getSlot(const TypeSystem::TypeID& tid) const;
    ServiceSlot* getSlot(const TypeSystem::TypeID& tid);

    std::unordered_map<TypeSystem::TypeID, ServiceSlot> _slots;  ///< Type -> slot mapping
    uint32_t _nextSlotIndex = 0;                                 ///< Counter for unique slot indices
};

}  // namespace Core
}  // namespace EntropyEngine
