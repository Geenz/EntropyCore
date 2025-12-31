/**
 * @file Reflection.h
 * @brief Compile-time reflection system for Entropy Engine
 *
 * This header provides a comprehensive reflection system that generates type information
 * at compile-time while maintaining runtime API compatibility. The system supports
 * field introspection, type metadata, and safe field value access.
 *
 * Key Features:
 * - Compile-time type registration via macros
 * - Runtime field introspection and value access
 * - Type-safe field value retrieval
 * - Type information for registered types
 * - Automatic field offset calculation
 * - Support for complex template types
 *
 * It's recommended to use this system for specific fields you want to track and serialize.
 * For example, serialization for network applications, or to disk.
 * You should only use this for the fields you care about serializing as well.  Hence, the manual registration.
 * This ensures that you only pay for what you need.
 *
 * Basic Usage:
 * @code
 * class MyClass {
 * public:
 *     ENTROPY_REGISTER_TYPE(MyClass);
 *
 *     ENTROPY_FIELD(int, value);
 *     ENTROPY_FIELD(double, ratio) = 0.5d;
 *     ENTROPY_FIELD(std::string, name);
 * };
 *
 * // Access reflection information
 * const auto* typeInfo = TypeInfo::get<MyClass>();
 * const auto& fields = typeInfo->getFields();
 *
 * MyClass instance;
 * for (const auto& field : fields) {
 *     if (field.name == "value") {
 *         auto value = TypeInfo::get_field_value<int>(&instance, field);
 *         if (value) {
 *             std::cout << "Value: " << *value << std::endl;
 *         }
 *     }
 * }
 * @endcode
 */

#pragma once

#include <algorithm>  // For std::reverse
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "TypeID.h"

namespace EntropyEngine
{
namespace Core
{
namespace TypeSystem
{

/**
 * @brief Information about a reflected field in a type
 *
 * Contains metadata to access a field: name, type, and memory offset.
 * Created automatically by ENTROPY_FIELD macro.
 */
struct FieldInfo
{
    /**
     * @brief The name of the field as a string view
     */
    std::string_view name;

    /**
     * @brief Type identification for the field
     */
    TypeID type;

    /**
     * @brief Memory offset of the field within the containing object
     */
    size_t offset;
};

/**
 * @brief Internal implementation details for the reflection system
 */
namespace detail
{
/**
 * @brief Placeholder for future constexpr field infrastructure
 *
 * This space is reserved for compile-time field accumulation
 * systems.
 */
// Future: constexpr field infrastructure will go here

/**
 * @brief Node in the linked list of field information
 *
 * Used during static initialization to collect field metadata
 * registered via ENTROPY_FIELD macros. Forms a singly-linked
 * list that gets converted to a vector at runtime.
 *
 * @note This is an implementation detail and may change in future versions
 */
struct FieldInfoNode
{
    const FieldInfo data;           ///< Field metadata
    FieldInfoNode* next = nullptr;  ///< Pointer to next node in list
};

/**
 * @brief Head pointer for the field registration linked list
 * @tparam T The type whose fields are being registered
 *
 * Each type T gets its own static linked list head for collecting
 * field information during static initialization.
 */
template <typename T>
struct FieldListHead
{
    inline static FieldInfoNode* head = nullptr;
};
}  // namespace detail

/**
 * @brief Compile-time type information collector
 * @tparam T The type to collect information for
 *
 * This template provides access to type metadata by combining
 * compile-time type name resolution with runtime field collection.
 * Types using ENTROPY_REGISTER_TYPE will have their information
 * generated at compile time where possible.
 *
 * @note This is an internal helper structure used by TypeInfo
 */
template <typename T>
struct compile_time_type_info
{
    /**
     * @brief Get the TypeID for this type
     * @return TypeID object uniquely identifying type T
     */
    static TypeID get_type_id() {
        return createTypeId<T>();
    }

    /**
     * @brief Compile-time type name from ENTROPY_REGISTER_TYPE
     */
    static constexpr std::string_view name = T::getStaticTypeName();

    /**
     * @brief Collect field information from the registration system
     * @return Vector of FieldInfo objects for all registered fields
     *
     * Traverses linked list and reverses to match declaration order.
     * O(n), called once per type.
     */
    static std::vector<FieldInfo> get_fields() {
        std::vector<FieldInfo> fields;
        for (auto* node = detail::FieldListHead<T>::head; node != nullptr; node = node->next) {
            fields.push_back(node->data);
        }
        std::reverse(fields.begin(), fields.end());
        return fields;
    }
};

/**
 * @brief Runtime type information container with field introspection
 *
 * Primary interface for runtime reflection. Features static instance
 * caching, type-safe field access, and automatic offset calculation.
 */
class TypeInfo
{
private:
    TypeID m_id;                      ///< Unique type identifier
    std::string_view m_name;          ///< Human-readable type name
    std::vector<FieldInfo> m_fields;  ///< List of reflected fields

    // Legacy runtime registration (for backward compatibility during transition)
    inline static std::map<TypeID, std::unique_ptr<TypeInfo>> s_registry;
    inline static std::map<TypeID, std::function<std::unique_ptr<TypeInfo>()>> s_factories;

    /**
     * @brief Private constructor for TypeInfo instances
     */
    TypeInfo(TypeID id, std::string_view name, std::vector<FieldInfo>&& fields)
        : m_id(id), m_name(name), m_fields(std::move(fields)) {}

    /**
     * @brief Create TypeInfo from compile-time type information
     * @tparam T The type to create TypeInfo for
     * @return Constructed TypeInfo instance
     */
    template <typename T>
    static TypeInfo create_from_constexpr() {
        return TypeInfo{compile_time_type_info<T>::get_type_id(), compile_time_type_info<T>::name,
                        compile_time_type_info<T>::get_fields()};
    }

public:
    /**
     * @brief Get the unique type identifier
     * @return TypeID for this type
     */
    TypeID getID() const {
        return m_id;
    }

    /**
     * @brief Get the human-readable type name
     * @return Type name as string_view (zero allocation)
     */
    std::string_view getName() const {
        return m_name;
    }

    /**
     * @brief Get all reflected fields for this type
     * @return Const reference to vector of FieldInfo objects
     *
     * Fields are returned in declaration order. The vector is
     * constructed once per type and cached for efficiency.
     */
    const std::vector<FieldInfo>& getFields() const {
        return m_fields;
    }

    /**
     * @brief Get TypeInfo for a specific type T
     * @tparam T The type to get reflection information for
     * @return Pointer to TypeInfo instance, or nullptr if not registered
     *
     * This is the primary entry point for accessing type reflection information.
     * For types registered with ENTROPY_REGISTER_TYPE, this function uses a
     * compile-time path with static instance caching. Legacy types
     * fall back to the runtime registration system.
     *
     * @code
     * // Get reflection info for a registered type
     * const auto* info = TypeInfo::get<MyClass>();
     * if (info) {
     *     std::cout << "Type: " << info->getName() << std::endl;
     *     std::cout << "Fields: " << info->getFields().size() << std::endl;
     * }
     * @endcode
     */
    template <typename T>
    static const TypeInfo* get() {
        // Use compile-time approach for types with getStaticTypeName
        if constexpr (requires { T::getStaticTypeName(); }) {
            static const TypeInfo instance = create_from_constexpr<T>();
            return &instance;
        }

        // Fallback to legacy runtime registration for old types
        const auto typeId = createTypeId<T>();
        if (const auto it = s_registry.find(typeId); it != s_registry.end()) {
            return it->second.get();
        }

        if (const auto it = s_factories.find(typeId); it != s_factories.end()) {
            std::unique_ptr<TypeInfo> newInstance = it->second();
            const TypeInfo* ptr = newInstance.get();
            s_registry[typeId] = std::move(newInstance);
            return ptr;
        }
        return nullptr;
    }

    /**
     * @brief Safely retrieve a field value from an object instance
     * @tparam T The expected type of the field value
     * @param obj Pointer to the object instance (must not be null)
     * @param field FieldInfo describing the field to access
     * @return Optional containing the field value, or nullopt if type mismatch
     *
     * This function provides type-safe access to field values using the
     * field offset information. Type safety is enforced by comparing the
     * requested type T with the field's registered type.
     *
     * Safety Features:
     * - Type validation prevents incorrect casts
     * - Uses std::optional to handle type mismatches gracefully
     * - Direct memory access
     *
     * @warning The object pointer must be valid and point to an instance
     *          of the type that owns the field. No bounds checking is performed.
     *
     * @code
     * MyClass instance;
     * const auto* typeInfo = TypeInfo::get<MyClass>();
     *
     * for (const auto& field : typeInfo->getFields()) {
     *     if (field.name == "myIntField") {
     *         auto value = TypeInfo::get_field_value<int>(&instance, field);
     *         if (value) {
     *             std::cout << "Field value: " << *value << std::endl;
     *         } else {
     *             std::cout << "Type mismatch!" << std::endl;
     *         }
     *     }
     * }
     * @endcode
     */
    template <typename T>
    static std::optional<T> get_field_value(const void* obj, const FieldInfo& field) {
        if (createTypeId<T>() != field.type) {
            return std::nullopt;
        }
        const char* obj_bytes = static_cast<const char*>(obj);
        return *reinterpret_cast<const T*>(obj_bytes + field.offset);
    }

    template <typename T>
    friend struct _EntropyTypeRegistrar;
};

/**
 * @brief Legacy type registrar for backward compatibility
 * @tparam T The type to register using the old runtime system
 *
 * Fallback for types without compile-time support. Will be deprecated.
 */
template <typename T>
struct _EntropyTypeRegistrar
{
    _EntropyTypeRegistrar() {
        // Only register if type doesn't support the new compile-time approach
        if constexpr (!requires { T::getStaticTypeName(); }) {
            auto typeId = createTypeId<T>();
            TypeInfo::s_factories[typeId] = [typeId]() {
                std::vector<FieldInfo> fields;
                for (auto* node = detail::FieldListHead<T>::head; node != nullptr; node = node->next) {
                    fields.push_back(node->data);
                }
                std::reverse(fields.begin(), fields.end());
                return std::unique_ptr<TypeInfo>(new TypeInfo(typeId, "Unknown", std::move(fields)));
            };
        }
    }
};

/**
 * @def ENTROPY_REGISTER_TYPE(TypeName)
 * @brief Register a type for compile-time reflection
 * @param TypeName The name of the type to register (unquoted)
 *
 * This macro must be placed in the public section of any class that
 * wants to participate in the reflection system. It provides:
 * - Compile-time type name access via getStaticTypeName()
 * - Instance method type() for getting TypeID
 * - Automatic integration with the TypeInfo system
 *
 * Features:
 * - Compile-time type name access
 * - Automatic TypeInfo generation with static caching
 * - Full namespace qualification to avoid naming conflicts
 *
 * Usage:
 * @code
 * class MyClass {
 * public:
 *     ENTROPY_REGISTER_TYPE(MyClass);
 *
 *     // Your class members here...
 * };
 * @endcode
 *
 * Generated members:
 * - `static constexpr std::string_view getStaticTypeName()`
 * - `TypeID type() const`
 * - `using OwnerType = TypeName` (private, for field registration)
 */
#define ENTROPY_REGISTER_TYPE(TypeName)                                     \
private:                                                                    \
    using OwnerType = TypeName;                                             \
                                                                            \
public:                                                                     \
    static constexpr std::string_view getStaticTypeName() {                 \
        return #TypeName;                                                   \
    }                                                                       \
    ::EntropyEngine::Core::TypeSystem::TypeID type() const {                \
        return ::EntropyEngine::Core::TypeSystem::createTypeId<TypeName>(); \
    }

/**
 * @def ENTROPY_FIELD(Type, Name)
 * @brief Register a field for runtime reflection
 * @param Type The type of the field (fully qualified if needed)
 * @param Name The name of the field (unquoted)
 *
 * This macro must be placed where you would normally declare a class member.
 * It simultaneously declares the member variable and registers it for reflection.
 * The macro must be used within a class that has ENTROPY_REGISTER_TYPE.
 *
 * Features:
 * - Automatic field offset calculation using offsetof()
 * - Type-safe field registration with full type information
 * - Integration with the linked list collection system
 * - Maintains declaration order in reflection metadata
 *
 * Requirements:
 * - Must be used in a class with ENTROPY_REGISTER_TYPE
 * - Field names must be valid C++ identifiers
 * - Types must be complete at the point of registration
 *
 * Usage:
 * @code
 * class MyClass {
 * public:
 *     ENTROPY_REGISTER_TYPE(MyClass);
 *
 *     ENTROPY_FIELD(int, health);
 *     ENTROPY_FIELD(std::string, name);
 *     ENTROPY_FIELD(glm::vec3, position);
 *
 * private:
 *     ENTROPY_FIELD(bool, isActive);  // Private fields also supported
 * };
 * @endcode
 *
 * Generated components:
 * - The actual member variable: `Type Name`
 * - Static registration helper structures (private)
 * - Automatic insertion into the field linked list
 *
 * @warning Field types containing commas (like `std::map<K,V>`) may require
 *          careful handling or typedef declarations
 */
#define ENTROPY_FIELD(Type, Name)                                                                             \
private:                                                                                                      \
    struct _EntropyFieldRegistrar_##Name                                                                      \
    {                                                                                                         \
        _EntropyFieldRegistrar_##Name() {                                                                     \
            static ::EntropyEngine::Core::TypeSystem::detail::FieldInfoNode node{                             \
                {#Name, ::EntropyEngine::Core::TypeSystem::createTypeId<Type>(), offsetof(OwnerType, Name)}}; \
            node.next = ::EntropyEngine::Core::TypeSystem::detail::FieldListHead<OwnerType>::head;            \
            ::EntropyEngine::Core::TypeSystem::detail::FieldListHead<OwnerType>::head = &node;                \
        }                                                                                                     \
    };                                                                                                        \
    inline static _EntropyFieldRegistrar_##Name _entropy_field_registrar_##Name;                              \
                                                                                                              \
public:                                                                                                       \
    Type Name

}  // namespace TypeSystem
}  // namespace Core
}  // namespace EntropyEngine
