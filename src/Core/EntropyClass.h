//
// Created by goodh on 9/17/2025.
//

#ifndef ENTROPYCORE_ENTROPYCLASS_H
#define ENTROPYCORE_ENTROPYCLASS_H

#include <cstdint>
#include "TypeSystem/TypeID.h"

/**
 * @brief Convenience macro for implementing EntropyObject virtual methods
 *
 * Use this macro in classes derived from EntropyObject to automatically implement
 * className() and classHash() with compile-time class name stringification.
 *
 * This macro intentionally does NOT provide toString() or description() implementations,
 * allowing you to provide custom implementations or inherit EntropyObject's defaults.
 *
 * @code
 * class MyObject : public EntropyObject {
 *     ENTROPY_CLASS_BODY(MyObject)
 *     // Optionally override toString() and description() with custom implementations
 * };
 * @endcode
 */
#define ENTROPY_CLASS_BODY(ClassName) \
    public: \
        /* Virtual instance overrides (no toString/description) */ \
        const char* className() const noexcept override \
        { \
            return #ClassName; \
        } \
        uint64_t classHash() const noexcept override \
        { \
            static const uint64_t hash = static_cast<uint64_t>(::EntropyEngine::Core::TypeSystem::createTypeId<ClassName>().id); \
            return hash; \
        }


#endif //ENTROPYCORE_ENTROPYCLASS_H