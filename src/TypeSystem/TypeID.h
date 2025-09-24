/**
 * @file TypeID.h
 * @brief Cross-platform type identification system for Entropy Engine
 * 
 * This header provides a stable, hashable type identification system using boost::type_index.
 * TypeID objects can be used for type comparison, hashing, and runtime type identification
 * while maintaining consistent behavior across different platforms and compilers.
 */

#pragma once

#include <boost/type_index.hpp>
#include <string>
#include <compare>
#include <functional>
#include <cstdint>

#ifndef ENTROPY_ENABLE_RTTI
#define ENTROPY_ENABLE_RTTI 0
#endif

#ifndef ENTROPY_TYPEID_INCLUDE_NAME
#define ENTROPY_TYPEID_INCLUDE_NAME 0
#endif

namespace EntropyEngine {
    namespace Core {
        namespace TypeSystem {

            /**
             * @brief A cross-platform type identifier with stable hashing and comparison
             * 
             * TypeID provides a consistent way to identify types at runtime across different
             * platforms and compilers. It uses boost::type_index internally to generate
             * stable hash codes and human-readable type names.
             * 
             * Features:
             * - Stable hash codes across compilation units
             * - Human-readable type names with template parameters (optionally compiled in)
             * - Supports comparison operations
             * - Compatible with standard containers (via std::hash specialization)
             * 
             * @note TypeID objects are immutable and safe to use across thread boundaries
             * 
             * Example usage:
             * @code
             * auto intTypeId = createTypeId<int>();
             * auto floatTypeId = createTypeId<float>();
             * 
             * if (intTypeId == floatTypeId) {
             *     // Never executed - different types
             * }
             * @endcode
             */
            struct TypeID {
                /**
                 * @brief 64-bit canonical hash identifier for the type
                 * 
                 * Derived from boost::type_index::hash_code() and normalized to
                 * uint64_t for cross-ABI stability within a build.
                 */
                uint64_t id;
                
                /**
                 * @brief Human-readable name of the type
                 * 
                 * Contains the canonical type name including template parameters.
                 * For example: "glm::vec<3, float>" or "std::vector<int>".
                 */
                std::string name;

                /**
                 * @brief Three-way comparison operator for ordering TypeIDs
                 * @param other The TypeID to compare against
                 * @return std::strong_ordering result based on hash comparison
                 */
                auto operator<=>(const TypeID& other) const { return id <=> other.id; }
                
                /**
                 * @brief Equality comparison operator
                 * @param other The TypeID to compare against
                 * @return true if both TypeIDs represent the same type
                 */
                bool operator==(const TypeID& other) const { return id == other.id; }

                /**
                 * @brief Get the human-readable type name
                 * @return The pretty-printed name of the type with template parameters
                 * 
                 * This method returns the same value as the `name` member but provides
                 * a method-based interface for consistency with other APIs.
                 */
                [[nodiscard]] std::string prettyName() const {
                    return name;
                }
            };

            /**
             * @brief Create a TypeID for a given type T
             * @tparam T The type to create an identifier for
             * @return TypeID object uniquely identifying type T
             * @noexcept This function never throws exceptions
             * 
             * This function generates a stable TypeID for any given type using boost::type_index.
             * The resulting TypeID will be identical for the same type across different
             * compilation units and function calls.
             * 
             * Template parameters and typedefs are resolved to their canonical forms:
             * - `std::string` becomes the underlying template instantiation
             * - `glm::quat` becomes `glm::qua<float>`
             * - Template parameters are preserved in the name
             * 
             * Performance characteristics:
             * - O(1) hash generation
             * - Small string allocation for type name
             * - Inlined for zero function call overhead
             * 
             * @code
             * // Basic types
             * auto intId = createTypeId<int>();
             * auto floatId = createTypeId<float>();
             * 
             * // Template types
             * auto vectorId = createTypeId<std::vector<int>>();
             * auto quatId = createTypeId<glm::quat>(); // Shows as "glm::qua<float>"
             * 
             * // Custom types
             * auto customId = createTypeId<MyCustomClass>();
             * @endcode
             */
            // Cached, allocation-free type id for T. This avoids RTTI/string work in hot paths.
            template <typename T>
            [[nodiscard]] inline const TypeID& typeIdOf() noexcept {
                const auto index = boost::typeindex::type_id<T>();
#if ENTROPY_TYPEID_INCLUDE_NAME
                static const TypeID k{ static_cast<uint64_t>(index.hash_code()), index.pretty_name() };
#else
                static const TypeID k{ static_cast<uint64_t>(index.hash_code()), std::string() };
#endif
                return k;
            }

            template <typename T>
            [[nodiscard]] inline TypeID createTypeId() noexcept {
                // Return the cached instance by value (cheap copy: two words + small string empty)
                return typeIdOf<T>();
            }

            /**
             * @brief Create a TypeID for the dynamic type of a given object reference
             * @tparam T The static type of the object (can be a base class)
             * @param obj Reference to the object to inspect at runtime
             * @return TypeID representing the dynamic type of obj
             */
#if ENTROPY_ENABLE_RTTI
            template <typename T>
            [[nodiscard]] inline TypeID createTypeIdRuntime(const T& obj) noexcept {
                const auto& index = boost::typeindex::type_id_runtime(obj);
                std::string pretty_name = index.pretty_name();
                return { index.hash_code(), std::move(pretty_name) };
            }
#endif

        } // namespace TypeSystem
    } // namespace Core
} // namespace EntropyEngine

/**
 * @brief Standard library hash specialization for TypeID
 * 
 * This specialization allows TypeID objects to be used as keys in standard
 * containers like std::unordered_map and std::unordered_set.
 */
namespace std {
    /**
     * @brief Hash function specialization for EntropyEngine::Core::TypeSystem::TypeID
     * 
     * Provides a hash function for TypeID objects by using the pre-computed
     * hash value from boost::type_index. This ensures consistent hashing behavior.
     * 
     * Example usage:
     * @code
     * std::unordered_map<TypeID, std::string> typeNames;
     * typeNames[createTypeId<int>()] = "Integer Type";
     * 
     * std::unordered_set<TypeID> registeredTypes;
     * registeredTypes.insert(createTypeId<float>());
     * @endcode
     */
    template <>
    struct hash<EntropyEngine::Core::TypeSystem::TypeID> {
        /**
         * @brief Compute hash value for a TypeID
         * @param typeId The TypeID to hash
         * @return Pre-computed hash value from boost::type_index
         * @noexcept This operation never throws
         */
        size_t operator()(const EntropyEngine::Core::TypeSystem::TypeID& typeId) const noexcept {
            return static_cast<size_t>(typeId.id);
        }
    };
} // namespace std

