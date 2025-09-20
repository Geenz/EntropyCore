//
// Created by goodh on 9/17/2025.
//

#ifndef ENTROPYCORE_ENTROPYCLASS_H
#define ENTROPYCORE_ENTROPYCLASS_H

#include <string>
#include <cstdint>
#include <functional>
#include "TypeSystem/TypeID.h"

#define ENTROPY_CLASS(ClassName) \
    public: \
        /* Virtual instance overrides */ \
        const char* className() const noexcept override \
        { \
            return #ClassName; \
        } \
        uint64_t classHash() const noexcept override \
        { \
            static const uint64_t hash = static_cast<uint64_t>(::EntropyEngine::Core::TypeSystem::createTypeId<ClassName>().id); \
            return hash; \
        } \
        std::string toString() const override \
        { \
            return #ClassName; \
        } \
        std::string description() const override \
        { \
            return #ClassName; \
        } \
    private:

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