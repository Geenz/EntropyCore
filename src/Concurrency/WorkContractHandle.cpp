/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

#include "WorkContractHandle.h"
#include "WorkContractGroup.h"
#include "../TypeSystem/TypeID.h"
#include <format>

namespace EntropyEngine {
namespace Core {
namespace Concurrency {

    ScheduleResult WorkContractHandle::schedule() {
        auto* group = handleOwnerAs<WorkContractGroup>();
        if (!group) return ScheduleResult::Invalid;
        return group->scheduleContract(*this);
    }
    
    ScheduleResult WorkContractHandle::unschedule() {
        auto* group = handleOwnerAs<WorkContractGroup>();
        if (!group) return ScheduleResult::Invalid;
        return group->unscheduleContract(*this);
    }
    
    bool WorkContractHandle::valid() const {
        auto* group = handleOwnerAs<WorkContractGroup>();
        return group && group->isValidHandle(*this);
    }
    
    void WorkContractHandle::release() {
        if (auto* group = handleOwnerAs<WorkContractGroup>()) {
            group->releaseContract(*this);
        }
        // Clear stamped identity to make subsequent calls fast no-ops
        HandleAccess::clear(*this);
    }
    
    bool WorkContractHandle::isScheduled() const {
        auto* group = handleOwnerAs<WorkContractGroup>();
        if (!group) return false;
        return group->getContractState(*this) == ContractState::Scheduled;
    }
    
    bool WorkContractHandle::isExecuting() const {
        auto* group = handleOwnerAs<WorkContractGroup>();
        if (!group) return false;
        return group->getContractState(*this) == ContractState::Executing;
    }

uint64_t WorkContractHandle::classHash() const noexcept {
    static const uint64_t hash = static_cast<uint64_t>(EntropyEngine::Core::TypeSystem::createTypeId<WorkContractHandle>().id);
    return hash;
}

std::string WorkContractHandle::toString() const {
    if (hasHandle()) {
        return std::format("{}@{}(owner={}, idx={}, gen={})",
                           className(), static_cast<const void*>(this), handleOwner(), handleIndex(), handleGeneration());
    }
    return std::format("{}@{}(invalid)", className(), static_cast<const void*>(this));
}

} // namespace Concurrency
} // namespace Core
} // namespace EntropyEngine