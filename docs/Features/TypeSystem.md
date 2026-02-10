---
title: Type System
---

# Feature: Type System

The Entropy Type System provides compile-time reflection, unique type identification (`TypeID`), and field introspection for C++ classes.

## Overview

The system uses macros to generate metadata at compile-time, allowing for efficient:
*   **Serialization**: Automatically traversing fields for saving/loading.
*   **Scripting**: Exposing C++ types to scripting languages (e.g., Lua/Python).
*   **Scripting**: Binding C++ objects to scripting languages.

## Usage

### 1. Registration
To make a class reflectable, use `ENTROPY_REGISTER_TYPE` and `ENTROPY_FIELD` within the class definition.

```cpp
#include <EntropyCore/TypeSystem/Reflection.h>

class UserProfile {
public:
    // 1. Register the type
    ENTROPY_REGISTER_TYPE(UserProfile);

    // 2. Register fields
    ENTROPY_FIELD(float, creditBalance);
    ENTROPY_FIELD(std::string, username);
    ENTROPY_FIELD(glm::vec3, lastLocation);

    // Private fields can also be registered
private:
    ENTROPY_FIELD(bool, isAdmin);
};
```

### 2. Introspection
Query type information at runtime using `TypeInfo`.

```cpp
// Get metadata
const TypeInfo* info = TypeInfo::get<UserProfile>();

if (info) {
    Log::info("Inspecting type: {}", info->getName());

    UserProfile profile;
    profile.creditBalance = 100.0f;

    // Iterate fields
    for (const auto& field : info->getFields()) {
        Log::info("Field: {} (Offset: {})", field.name, field.offset);

        // Type-safe value access
        if (field.name == "creditBalance") {
            auto val = TypeInfo::get_field_value<float>(&profile, field);
            if (val) {
                Log::info("Balance is {}", *val);
            }
        }
    }
}
```

## Key Concepts

### `TypeID`
A 64-bit stable hash that uniquely identifies a type.

```cpp
TypeID id = createTypeId<UserProfile>();
```

### Extensions
You can register fields as "Extensions" (e.g., for USD schema augmentation) using `ENTROPY_FIELD_EXTENSION`.

```cpp
ENTROPY_FIELD_EXTENSION(float, lightIntensity);
```
