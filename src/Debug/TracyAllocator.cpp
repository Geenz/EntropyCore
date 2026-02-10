/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/**
 * @file TracyAllocator.cpp
 * @brief Global allocator hooks for Tracy memory profiling.
 *
 * Overrides global new/delete to track ALL memory allocations in Tracy's
 * memory profiler with full callstack visibility.
 *
 * IMPORTANT: Do NOT use TracyAllocN/TracyFreeN elsewhere for allocations
 * that go through new/delete - it will cause double-tracking errors.
 */

#include <cstdlib>
#include <new>
#include <tracy/Tracy.hpp>

void* operator new(std::size_t size) {
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    TracyAlloc(ptr, size);
    return ptr;
}

void* operator new[](std::size_t size) {
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    TracyAlloc(ptr, size);
    return ptr;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* ptr = std::malloc(size);
    if (ptr) TracyAlloc(ptr, size);
    return ptr;
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    void* ptr = std::malloc(size);
    if (ptr) TracyAlloc(ptr, size);
    return ptr;
}

void operator delete(void* ptr) noexcept {
    if (ptr) TracyFree(ptr);
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    if (ptr) TracyFree(ptr);
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    if (ptr) TracyFree(ptr);
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    if (ptr) TracyFree(ptr);
    std::free(ptr);
}

// C++17 aligned allocation overloads
void* operator new(std::size_t size, std::align_val_t al) {
    void* ptr = nullptr;
#if defined(_WIN32)
    ptr = _aligned_malloc(size, static_cast<std::size_t>(al));
#else
    if (posix_memalign(&ptr, static_cast<std::size_t>(al), size) != 0) ptr = nullptr;
#endif
    if (!ptr) throw std::bad_alloc();
    TracyAlloc(ptr, size);
    return ptr;
}

void* operator new[](std::size_t size, std::align_val_t al) {
    void* ptr = nullptr;
#if defined(_WIN32)
    ptr = _aligned_malloc(size, static_cast<std::size_t>(al));
#else
    if (posix_memalign(&ptr, static_cast<std::size_t>(al), size) != 0) ptr = nullptr;
#endif
    if (!ptr) throw std::bad_alloc();
    TracyAlloc(ptr, size);
    return ptr;
}

void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    void* ptr = nullptr;
#if defined(_WIN32)
    ptr = _aligned_malloc(size, static_cast<std::size_t>(al));
#else
    if (posix_memalign(&ptr, static_cast<std::size_t>(al), size) != 0) ptr = nullptr;
#endif
    if (ptr) TracyAlloc(ptr, size);
    return ptr;
}

void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    void* ptr = nullptr;
#if defined(_WIN32)
    ptr = _aligned_malloc(size, static_cast<std::size_t>(al));
#else
    if (posix_memalign(&ptr, static_cast<std::size_t>(al), size) != 0) ptr = nullptr;
#endif
    if (ptr) TracyAlloc(ptr, size);
    return ptr;
}

void operator delete(void* ptr, std::align_val_t) noexcept {
    if (ptr) TracyFree(ptr);
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
    if (ptr) TracyFree(ptr);
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    if (ptr) TracyFree(ptr);
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    if (ptr) TracyFree(ptr);
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
