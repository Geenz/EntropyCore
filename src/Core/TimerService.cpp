/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Engine project.
 */

#include "TimerService.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../Concurrency/WorkService.h"
#include "../Logging/Logger.h"

namespace EntropyEngine
{
namespace Core
{

TimerService::TimerService() : TimerService(Config{}) {}

TimerService::TimerService(const Config& config) : _config(config) {}

TimerService::~TimerService() {
    // Ensure clean shutdown
    if (state() != ServiceState::Unloaded && state() != ServiceState::Stopped) {
        stop();
        unload();
    }
}

std::vector<TypeSystem::TypeID> TimerService::dependsOnTypes() const {
    return {TypeSystem::createTypeId<Concurrency::WorkService>()};
}

void TimerService::load() {
    // Note: Initial refcount=1 is set by EntropyObject base class (see EntropyObject.h:56)
    _workContractGroup = new Concurrency::WorkContractGroup(_config.workContractGroupSize);
}

void TimerService::start() {
    // Note: WorkService must be injected via setWorkService() before calling start()
    // The application is responsible for injecting dependencies
}

void TimerService::stop() {
    // Early exit if already unloaded (WorkContractGroup destroyed)
    if (!_workContractGroup) {
        return;
    }

    std::unordered_map<uint64_t, std::shared_ptr<TimerData>> entries;
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        entries.swap(_timers);
    }

    // Handles are released with no lock held: release() runs the group's capacity callbacks.
    std::vector<Concurrency::WorkContractHandle> pending;
    size_t cancelledCount = 0;
    for (auto& [id, entry] : entries) {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->cancelled.exchange(true, std::memory_order_acq_rel) && !entry->done.load(std::memory_order_acquire)) {
            ++cancelledCount;
        }
        if (entry->awaitingCapacity) {
            entry->awaitingCapacity = false;
            _awaitingCapacityCount.fetch_sub(1, std::memory_order_acq_rel);
        }
        pending.push_back(std::exchange(entry->pending, Concurrency::WorkContractHandle{}));
    }
    for (auto& handle : pending) {
        if (handle.unschedule() == Concurrency::ScheduleResult::NotScheduled) {
            handle.release();
        }
    }
    pending.clear();
    entries.clear();
    releaseOrphans();

    // Unregister WorkContractGroup from WorkService; waits for executing bodies. Detached, so a
    // later scheduleTimer() fails like one before start instead of arming a timer nothing runs.
    if (_workService && _workContractGroup) {
        _workService->removeWorkContractGroup(_workContractGroup);
    }
    _workService = nullptr;
    ENTROPY_LOG_INFO(std::format("TimerService: stopped, {} timers cancelled", cancelledCount));
}

void TimerService::unload() {
    // Contract bodies hold their own shared_ptr<TimerData>, so clearing the map is safe
    // while bodies are still running.
    if (_workContractGroup && _capacityCallback) {
        _workContractGroup->removeOnCapacityAvailable(*_capacityCallback);
        _capacityCallback.reset();
    }

    std::unordered_map<uint64_t, std::shared_ptr<TimerData>> entries;
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        entries.swap(_timers);
    }
    entries.clear();
    _awaitingCapacityCount.store(0, std::memory_order_release);

    // Object will be deleted when all references are released (including WorkService snapshots)
    if (_workContractGroup) {
        releaseOrphans();
        _workContractGroup->release();
        _workContractGroup = nullptr;
    }

    _workService = nullptr;
}

void TimerService::setWorkService(Concurrency::WorkService* workService) {
    _workService = workService;

    if (_workService && _workContractGroup) {
        auto status = _workService->addWorkContractGroup(_workContractGroup);
        if (status != Concurrency::WorkService::GroupOperationStatus::Added) {
            throw std::runtime_error("Failed to register TimerService WorkContractGroup with WorkService");
        }

        // Contract completion frees capacity: retries the re-arms a full group refused.
        _capacityCallback = _workContractGroup->addOnCapacityAvailable([this]() { onCapacityAvailable(); });

        ENTROPY_LOG_INFO(std::format("TimerService: attached to WorkService, contract group capacity {}",
                                     _workContractGroup->capacity()));
    }
}

Timer TimerService::scheduleTimer(std::chrono::steady_clock::duration interval, Timer::WorkFunction work,
                                  bool repeating, Concurrency::ExecutionType executionType) {
    if (!_workContractGroup) {
        throw std::runtime_error("TimerService not loaded");
    }

    if (!_workService) {
        throw std::runtime_error("TimerService not started - WorkService not set");
    }

    // A repeating timer re-arms for now + interval; without a positive interval it would fire on
    // every pull and hold a worker.
    if (repeating && interval <= std::chrono::steady_clock::duration::zero()) {
        ENTROPY_LOG_ERROR("TimerService: repeating timer not armed, interval must be positive");
        return Timer();
    }

    auto entry = std::make_shared<TimerData>();
    const auto now = std::chrono::steady_clock::now();
    entry->fireTime = interval >= Timer::TimePoint::max() - now ? Timer::TimePoint::max() : now + interval;
    entry->interval = interval;
    entry->work = std::move(work);
    entry->repeating = repeating;
    entry->executionType = executionType;

    // Finished one-shots are dropped here; destroyed after the lock is released.
    std::vector<std::shared_ptr<TimerData>> finished;
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        for (auto it = _timers.begin(); it != _timers.end();) {
            if (it->second->done.load(std::memory_order_acquire)) {
                finished.push_back(std::move(it->second));
                it = _timers.erase(it);
            } else {
                ++it;
            }
        }
        entry->id = _nextTimerId++;
        _timers.emplace(entry->id, entry);
    }
    finished.clear();

    bool armed = true;
    Concurrency::WorkContractHandle orphan;
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->cancelled.load(std::memory_order_acquire)) {
            armed = arm(entry, entry->fireTime, orphan);
        }
    }
    if (orphan.valid()) {
        orphan.release();
    }
    if (!armed) {
        std::lock_guard<std::mutex> lock(_timersMutex);
        _timers.erase(entry->id);
        return Timer();
    }
    releaseOrphans();

    return Timer(this, entry->id, interval, repeating);
}

bool TimerService::arm(const std::shared_ptr<TimerData>& entry, Timer::TimePoint fireTime,
                       Concurrency::WorkContractHandle& orphan) {
    auto handle = _workContractGroup->createContract([this, entry]() { fire(entry); }, entry->executionType);
    if (!handle.valid()) {
        ENTROPY_LOG_ERROR(std::format("TimerService: timer {} not armed, work contract group is full", entry->id));
        return false;
    }

    entry->fireTime = fireTime;
    entry->pending = handle;
    const auto result = handle.scheduleAt(fireTime);
    if (result != Concurrency::ScheduleResult::Scheduled) {
        ENTROPY_LOG_ERROR(std::format("TimerService: timer {} not armed, scheduleAt failed ({})", entry->id,
                                      static_cast<int>(result)));
        entry->pending = Concurrency::WorkContractHandle{};
        orphan = handle;
        return false;
    }
    return true;
}

void TimerService::fire(const std::shared_ptr<TimerData>& entry) {
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->pending = Concurrency::WorkContractHandle{};
    }

    if (!entry->cancelled.load(std::memory_order_acquire) && entry->work) {
        entry->work();
    }

    if (!entry->repeating) {
        entry->done.store(true, std::memory_order_release);
        return;
    }

    // The next fire is armed only here, so a repeating timer never has two bodies in flight.
    Concurrency::WorkContractHandle orphan;
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (entry->cancelled.load(std::memory_order_acquire)) {
            return;
        }

        // Fires missed while the body ran are skipped.
        const Timer::Duration step = std::max(entry->interval, Timer::Duration(1));
        const auto now = std::chrono::steady_clock::now();
        Timer::TimePoint next =
            step >= Timer::TimePoint::max() - entry->fireTime ? Timer::TimePoint::max() : entry->fireTime + step;
        if (next <= now) {
            next += step * ((now - next) / step + 1);
        }

        if (!arm(entry, next, orphan)) {
            ENTROPY_LOG_ERROR(std::format("TimerService: timer {} re-arm failed, retrying on freed capacity", entry->id));
            entry->fireTime = next;
            entry->awaitingCapacity = true;
            _awaitingCapacityCount.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    if (orphan.valid()) {
        orphan.release();
    }
}

void TimerService::onCapacityAvailable() {
    if (_awaitingCapacityCount.load(std::memory_order_acquire) == 0) {
        return;
    }

    std::vector<std::shared_ptr<TimerData>> candidates;
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        candidates.reserve(_timers.size());
        for (const auto& [id, entry] : _timers) {
            candidates.push_back(entry);
        }
    }

    // Runs under the group's callback mutex: release() cannot run here, so failed arms are parked.
    std::vector<Concurrency::WorkContractHandle> orphans;
    for (const auto& entry : candidates) {
        Concurrency::WorkContractHandle orphan;
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (!entry->awaitingCapacity || entry->cancelled.load(std::memory_order_acquire)) {
                continue;
            }
            const auto fireTime = std::max(entry->fireTime, std::chrono::steady_clock::now());
            if (arm(entry, fireTime, orphan)) {
                entry->awaitingCapacity = false;
                _awaitingCapacityCount.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
        if (orphan.valid()) {
            orphans.push_back(orphan);
        }
    }

    if (!orphans.empty()) {
        std::lock_guard<std::mutex> lock(_timersMutex);
        _orphans.insert(_orphans.end(), orphans.begin(), orphans.end());
    }
}

void TimerService::releaseOrphans() {
    std::vector<Concurrency::WorkContractHandle> orphans;
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        orphans.swap(_orphans);
    }
    for (auto& handle : orphans) {
        handle.release();
    }
}

void TimerService::cancelTimer(uint64_t timerId) {
    std::shared_ptr<TimerData> entry;
    {
        std::lock_guard<std::mutex> lock(_timersMutex);
        auto it = _timers.find(timerId);
        if (it == _timers.end()) {
            return;
        }
        entry = std::move(it->second);
        _timers.erase(it);
    }

    Concurrency::WorkContractHandle pending;
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->cancelled.store(true, std::memory_order_release);
        pending = std::exchange(entry->pending, Concurrency::WorkContractHandle{});
        if (entry->awaitingCapacity) {
            entry->awaitingCapacity = false;
            _awaitingCapacityCount.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    // Executing: the body sees `cancelled` and does not re-arm. Released with no lock held.
    if (pending.unschedule() == Concurrency::ScheduleResult::NotScheduled) {
        pending.release();
    }
    releaseOrphans();
}

size_t TimerService::getActiveTimerCount() const {
    std::lock_guard<std::mutex> lock(_timersMutex);
    size_t activeCount = 0;
    for (const auto& [id, data] : _timers) {
        if (!data->cancelled.load(std::memory_order_acquire) && !data->done.load(std::memory_order_acquire)) {
            ++activeCount;
        }
    }
    return activeCount;
}

}  // namespace Core
}  // namespace EntropyEngine
