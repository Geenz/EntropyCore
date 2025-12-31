/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Core project.
 */

/**
 * @file INamed.h
 * @brief Debug naming interface for meaningful object identification
 *
 * This file provides an interface for objects that can be named for debugging.
 * Named objects provide human-readable identifiers in logs and debug output.
 */

#pragma once

#include <string>
#include <string_view>

namespace EntropyEngine
{
namespace Core
{
namespace Debug
{

/**
 * @brief Interface for objects that support debug naming
 *
 * Standardizes debug naming across objects. Implement when objects appear
 * in logs, debug output, or profiler traces.
 *
 * @code
 * class MyComponent : public INamed {
 *     std::string name;
 * public:
 *     void setName(std::string_view n) override { name = n; }
 *     std::string_view getName() const override { return name; }
 * };
 *
 * // Meaningful log output
 * LOG_INFO("Processing component: {}", component->getName());
 * // Output: "Processing component: PlayerHealthUI"
 * @endcode
 */
class INamed
{
public:
    virtual ~INamed() = default;

    /**
     * @brief Set the debug name for this object
     *
     * Assigns a name for use in logs and debug output. Choose descriptive
     * names that indicate purpose (e.g., "MainMenuUI", "PlayerHealthBar").
     *
     * @param name The debug name to assign (can be empty to clear)
     */
    virtual void setName(std::string_view name) = 0;

    /**
     * @brief Get the debug name of this object
     *
     * Returns the assigned name or empty string if unnamed.
     *
     * @return The current debug name (may be empty)
     *
     * @code
     * if (object->hasName()) {
     *     LOG_INFO("Found object: {}", object->getName());
     * } else {
     *     LOG_INFO("Found unnamed object at {}", static_cast<void*>(object));
     * }
     * @endcode
     */
    [[nodiscard]] virtual std::string_view getName() const = 0;

    /**
     * @brief Check if this object has a debug name set
     *
     * Useful for conditional formatting with fallback to addresses.
     *
     * @return true if a non-empty debug name is set
     *
     * @code
     * std::string getObjectDescription(const INamed* obj) {
     *     if (obj->hasName()) {
     *         return std::format("'{}'", obj->getName());
     *     } else {
     *         return std::format("<unnamed at {}>", static_cast<const void*>(obj));
     *     }
     * }
     * @endcode
     */
    [[nodiscard]] virtual bool hasName() const {
        return !getName().empty();
    }
};

/**
 * @brief Simple implementation of INamed that can be inherited
 *
 * Provides basic INamed implementation with string storage. Inherit to add
 * naming functionality without implementing the interface yourself.
 *
 * @code
 * // Direct inheritance
 * class MySystem : public Named {
 * public:
 *     MySystem() : Named("MySystem") {}
 * };
 *
 * // Multiple inheritance
 * class MyComponent : public Component, public Named {
 * public:
 *     MyComponent(std::string_view name) : Named(name) {}
 * };
 *
 * // Virtual inheritance for diamond patterns
 * class MyMultiBase : public virtual Named, public OtherBase {
 *     // Avoids naming conflicts through virtual inheritance
 * };
 * @endcode
 */
class Named : public virtual INamed
{
private:
    std::string _name;

public:
    Named() = default;
    explicit Named(std::string_view name) : _name(name) {}

    void setName(std::string_view name) override {
        _name = name;
    }

    [[nodiscard]] std::string_view getName() const override {
        return _name;
    }
};

}  // namespace Debug
}  // namespace Core
}  // namespace EntropyEngine
