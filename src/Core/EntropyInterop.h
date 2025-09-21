#pragma once
#include "RefObject.h"
#include <memory>

namespace EntropyEngine::Core {

template<typename T>
struct EntropyDeleter {
    void operator()(T* ptr) const noexcept {
        if (ptr) ptr->release();
    }
};

template<typename T>
std::shared_ptr<T> toSharedPtr(const RefObject<T>& ref) {
    T* ptr = ref.get();
    if (ptr) {
        ptr->retain();
        return std::shared_ptr<T>(ptr, EntropyDeleter<T>());
    }
    return nullptr;
}

template<typename T>
std::shared_ptr<T> wrapInSharedPtr(T* ptr) {
    if (ptr) {
        ptr->retain();
        return std::shared_ptr<T>(ptr, EntropyDeleter<T>());
    }
    return nullptr;
}

} // namespace EntropyEngine::Core