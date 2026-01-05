#pragma once
#include <memory>

#include "RefObject.h"

namespace EntropyEngine::Core
{

template <typename T>
struct EntropyDeleter
{
    void operator()(T* ptr) const noexcept {
        if (ptr) ptr->release();
    }
};

template <typename T>
std::shared_ptr<T> toSharedPtr(const RefObject<T>& ref) {
    T* ptr = ref.get();
    if (ptr) {
        ptr->retain();
        return std::shared_ptr<T>(ptr, EntropyDeleter<T>());
    }
    return nullptr;
}

template <typename T>
std::shared_ptr<T> wrapInSharedPtr(T* ptr) {
    if (ptr) {
        ptr->retain();
        return std::shared_ptr<T>(ptr, EntropyDeleter<T>());
    }
    return nullptr;
}

/**
 * @brief Converts a shared_ptr to a RefObject
 *
 * Creates a RefObject from a shared_ptr, retaining the object.
 * Useful for bridging code that uses shared_ptr to the RefObject model.
 *
 * @param sp Shared pointer to convert
 * @return RefObject wrapping the same pointer with an additional retain
 */
template <typename T>
RefObject<T> fromSharedPtr(const std::shared_ptr<T>& sp) {
    if (sp) {
        return RefObject<T>(retain, sp.get());
    }
    return {};
}

}  // namespace EntropyEngine::Core
