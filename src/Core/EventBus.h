/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file EventBus.h
 * @brief Type-safe publish-subscribe event system - because coupling is for train cars, not code
 *
 * This file contains a lightweight EventBus implementation designed for per-instance use.
 * Unlike traditional global event buses, this one is meant to be embedded in your objects -
 * every WorkGraph, game entity, or UI widget can have its own private event highway.
 */

#pragma once

#include <any>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace EntropyEngine
{
namespace Core
{

/**
 * @brief Type-safe publish-subscribe event system for decoupled communication
 *
 * EventBus implements a publish-subscribe pattern where components can publish events
 * and subscribe to specific event types without direct knowledge of each other.
 * Publishers and subscribers only need to share common event type definitions.
 *
 * Unlike traditional global event buses, this implementation is lightweight enough
 * to support thousands of instances - one per WorkGraph, game entity, or UI widget.
 * Memory usage remains manageable even with many instances.
 *
 * Key features:
 * - Type-safe: Can't accidentally subscribe to the wrong event type
 * - Thread-safe: Supports concurrent publishing from any thread
 * - Zero virtual functions: No vtable overhead
 * - Exception-safe: One bad handler won't crash the whole system
 * - Self-cleaning: Removes empty handler lists when unsubscribing
 *
 * Common use cases:
 * - Decoupling UI from game logic
 * - Progress notifications from long-running operations
 * - State change notifications in complex systems
 * - Any time you're tempted to add Yet Another Callback Parameter
 *
 * Complexity characteristics:
 * - Subscribe: O(1) amortized
 * - Publish: O(n) where n = subscribers for that event type
 *
 * @code
 * // Define your event types - just plain structs
 * struct PlayerHealthChanged {
 *     int oldHealth;
 *     int newHealth;
 *     bool isDead() const { return newHealth <= 0; }
 * };
 *
 * // Subscribe from anywhere
 * EventBus& bus = gameEntity.getEventBus();
 * auto healthId = bus.subscribe<PlayerHealthChanged>([this](const auto& e) {
 *     updateHealthBar(e.newHealth);
 *     if (e.isDead()) {
 *         showGameOverScreen();
 *     }
 * });
 *
 * // Publish from anywhere else
 * bus.publish(PlayerHealthChanged{100, 0});  // RIP player
 *
 * // Clean up when done
 * bus.unsubscribe<PlayerHealthChanged>(healthId);
 * @endcode
 */
class EventBus
{
public:
    using HandlerId = size_t;
    using EventHandler = std::function<void(const std::any&)>;

    EventBus() = default;
    ~EventBus() = default;

    // Move-only to prevent accidental copies
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    // Cannot move with mutex member
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    /**
     * @brief Sign up to receive a specific type of event - like subscribing to a newsletter
     *
     * Handler called for each published event. Returns ID for unsubscribing.
     * Thread-safe.
     *
     * @tparam EventType The event struct/class you want to receive
     * @param handler Your callback - lambda, function, or callable
     * @return A unique ID for this subscription (save it!)
     *
     * @code
     * // Simple lambda subscription
     * auto id = bus.subscribe<MouseClick>([](const MouseClick& e) {
     *     std::cout << "Click at (" << e.x << ", " << e.y << ")\n";
     * });
     *
     * // Capture local state
     * int clickCount = 0;
     * bus.subscribe<MouseClick>([&clickCount](const MouseClick& e) {
     *     clickCount++;
     *     if (clickCount >= 10) {
     *         unlockAchievement("ClickHappy");
     *     }
     * });
     *
     * // Member function binding
     * bus.subscribe<GameStateChanged>(
     *     std::bind(&UIManager::onGameStateChanged, this, std::placeholders::_1)
     * );
     * @endcode
     */
    template <typename EventType>
    HandlerId subscribe(std::function<void(const EventType&)> handler) {
        std::lock_guard<std::mutex> lock(_mutex);

        auto typeIndex = std::type_index(typeid(EventType));
        auto wrappedHandler = [handler](const std::any& event) {
            try {
                const auto& typedEvent = std::any_cast<const EventType&>(event);
                handler(typedEvent);
            } catch (const std::bad_any_cast&) {
                // Type mismatch - should not happen with correct usage
            }
        };

        HandlerId id = _nextHandlerId++;
        _handlers[typeIndex].emplace_back(id, std::move(wrappedHandler));

        return id;
    }

    /**
     * @brief Cancel your subscription - stop receiving these events
     *
     * Pass the ID from subscribe() to remove handler. Cleans up empty lists.
     * Thread-safe.
     *
     * @tparam EventType The same event type you subscribed to
     * @param handlerId The ID you got from subscribe()
     * @return true if successfully unsubscribed, false if ID wasn't found
     *
     * @code
     * // Always save your subscription IDs!
     * class GameUI {
     *     EventBus& bus;
     *     EventBus::HandlerId healthSubId;
     *
     *     void onEnable() {
     *         healthSubId = bus.subscribe<HealthChanged>([this](auto& e) {
     *             updateHealthBar(e.newHealth);
     *         });
     *     }
     *
     *     void onDisable() {
     *         bus.unsubscribe<HealthChanged>(healthSubId);
     *     }
     * };
     * @endcode
     */
    template <typename EventType>
    bool unsubscribe(HandlerId handlerId) {
        std::lock_guard<std::mutex> lock(_mutex);

        auto typeIndex = std::type_index(typeid(EventType));
        auto it = _handlers.find(typeIndex);
        if (it == _handlers.end()) {
            return false;
        }

        auto& handlers = it->second;
        for (auto handlerIt = handlers.begin(); handlerIt != handlers.end(); ++handlerIt) {
            if (handlerIt->first == handlerId) {
                handlers.erase(handlerIt);

                // Clean up empty handler list to save memory
                if (handlers.empty()) {
                    _handlers.erase(it);
                }
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Broadcast an event to all interested parties - fire and forget!
     *
     * Sends to all subscribers of this type. Handlers called synchronously.
     * Safe: copies handlers, catches exceptions. Thread-safe.
     *
     * @tparam EventType The event type you're publishing
     * @param event The event data to send
     *
     * @code
     * // Fire a simple event
     * bus.publish(LevelCompleted{currentLevel, score, timeElapsed});
     *
     * // Events can have methods
     * struct DamageEvent {
     *     Entity* target;
     *     int amount;
     *     DamageType type;
     *
     *     bool isLethal() const {
     *         return target->health <= amount;
     *     }
     * };
     *
     * bus.publish(DamageEvent{player, 50, DamageType::Fire});
     *
     * // Publishing to no subscribers is fine - nothing happens
     * bus.publish(ObscureDebugEvent{});  // No subscribers? No problem!
     * @endcode
     */
    template <typename EventType>
    void publish(const EventType& event) {
        std::vector<EventHandler> handlersToCall;

        {
            std::lock_guard<std::mutex> lock(_mutex);

            auto typeIndex = std::type_index(typeid(EventType));
            auto it = _handlers.find(typeIndex);
            if (it != _handlers.end()) {
                // Copy handlers to avoid holding lock during callbacks
                handlersToCall.reserve(it->second.size());
                for (const auto& [id, handler] : it->second) {
                    handlersToCall.push_back(handler);
                }
            }
        }

        // Call handlers outside of lock to prevent deadlock
        for (const auto& handler : handlersToCall) {
            try {
                handler(event);
            } catch (...) {
                // Swallow exceptions from handlers to prevent one bad handler
                // from breaking the entire event system
            }
        }
    }

    /**
     * @brief Count how many subscribers are listening for a specific event type
     *
     * Skip expensive work if nobody's listening. Good for debugging too.
     *
     * @tparam EventType The event type to check
     * @return Number of active subscribers for this event type
     *
     * @code
     * // Optimize expensive operations
     * if (bus.getSubscriberCount<DetailedPhysicsUpdate>() > 0) {
     *     // Only calculate detailed physics if someone cares
     *     auto details = calculateExpensivePhysicsDetails();
     *     bus.publish(DetailedPhysicsUpdate{details});
     * }
     * @endcode
     */
    template <typename EventType>
    size_t getSubscriberCount() const {
        std::lock_guard<std::mutex> lock(_mutex);

        auto typeIndex = std::type_index(typeid(EventType));
        auto it = _handlers.find(typeIndex);
        return (it != _handlers.end()) ? it->second.size() : 0;
    }

    /**
     * @brief Quick check if anyone is listening to anything at all
     *
     * @return true if any handlers are registered, false if completely empty
     */
    bool hasSubscribers() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return !_handlers.empty();
    }

    /**
     * @brief Count total subscriptions across all event types
     *
     * @return Sum of all subscriptions for all event types
     */
    size_t getTotalSubscriptions() const {
        std::lock_guard<std::mutex> lock(_mutex);
        size_t total = 0;
        for (const auto& [type, handlers] : _handlers) {
            total += handlers.size();
        }
        return total;
    }

    /**
     * @brief Nuclear option: remove all subscriptions for all event types
     *
     * Wipes clean. All handler IDs become invalid. For shutdown cleanup.
     * Thread-safe.
     */
    void clear() {
        std::lock_guard<std::mutex> lock(_mutex);
        _handlers.clear();
    }

    /**
     * @brief Estimate how much memory this EventBus is using
     *
     * Includes object + dynamic allocations. Close enough for profiling.
     *
     * @return Approximate bytes used by this EventBus
     *
     * @code
     * // Memory profiling
     * if (bus.getMemoryUsage() > 1024 * 1024) {  // 1MB
     *     LOG_WARN("EventBus using {}KB of memory!",
     *              bus.getMemoryUsage() / 1024);
     * }
     * @endcode
     */
    size_t getMemoryUsage() const {
        std::lock_guard<std::mutex> lock(_mutex);

        size_t usage = sizeof(*this);
        usage += _handlers.size() * (sizeof(std::type_index) + sizeof(std::vector<std::pair<HandlerId, EventHandler>>));

        for (const auto& [type, handlers] : _handlers) {
            usage += handlers.size() * (sizeof(HandlerId) + sizeof(EventHandler));
        }

        return usage;
    }

private:
    mutable std::mutex _mutex;  ///< Protects all operations (mutable for const methods)

    /// Type-erased storage: maps event types to lists of (id, handler) pairs
    std::unordered_map<std::type_index, std::vector<std::pair<HandlerId, EventHandler>>> _handlers;

    HandlerId _nextHandlerId = 1;  ///< Simple incrementing ID generator
};

}  // namespace Core
}  // namespace EntropyEngine
