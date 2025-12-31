/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Engine project.
 */

#include "Timer.h"

#include "TimerService.h"

namespace EntropyEngine
{
namespace Core
{

Timer::Timer(TimerService* service, Concurrency::WorkGraph::NodeHandle node, Duration interval, bool repeating)
    : _service(service), _node(std::move(node)), _interval(interval), _repeating(repeating), _valid(true) {}

Timer::Timer(Timer&& other) noexcept
    : _service(other._service),
      _node(std::move(other._node)),
      _interval(other._interval),
      _repeating(other._repeating),
      _valid(other._valid.load(std::memory_order_acquire)) {
    other._service = nullptr;
    other._valid.store(false, std::memory_order_release);
}

Timer& Timer::operator=(Timer&& other) noexcept {
    if (this != &other) {
        // Invalidate current timer first
        invalidate();

        // Transfer ownership
        _service = other._service;
        _node = std::move(other._node);
        _interval = other._interval;
        _repeating = other._repeating;
        _valid.store(other._valid.load(std::memory_order_acquire), std::memory_order_release);

        // Invalidate source
        other._service = nullptr;
        other._valid.store(false, std::memory_order_release);
    }
    return *this;
}

Timer::~Timer() {
    invalidate();
}

void Timer::invalidate() {
    // CAS to ensure exactly-once invalidation
    bool expected = true;
    if (_valid.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        if (_service) {
            _service->cancelTimer(_node);
        }
    }
}

bool Timer::isValid() const {
    return _valid.load(std::memory_order_acquire);
}

}  // namespace Core
}  // namespace EntropyEngine
