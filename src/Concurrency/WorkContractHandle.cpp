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

} // namespace Concurrency
} // namespace Core
} // namespace EntropyEngine