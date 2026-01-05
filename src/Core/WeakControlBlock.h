//
// Created by goodh on 9/17/2025.
//

#pragma once

#include <atomic>
#include <mutex>

namespace EntropyEngine::Core
{

class EntropyObject;

/**
 * @brief Control block for handling safe weak references to EntropyObjects
 *
 * This block allows WeakRef instances to safely check if the underlying object
 * is still alive. The block persists as long as either the object is alive OR
 * there are any WeakRefs pointing to it.
 */
struct WeakControlBlock
{
    // Number of references to this control block (1 for the object itself + N for WeakRefs)
    std::atomic<uint32_t> refCount{1};

    // Pointer to the object. synchronizes access via mutex.
    // Set to nullptr when the object is destroyed.
    EntropyObject* object = nullptr;

    // Mutex to synchronize object destruction vs WeakRef locking
    std::mutex mutex;

    explicit WeakControlBlock(EntropyObject* obj) : object(obj) {}

    void retain() {
        refCount.fetch_add(1, std::memory_order_relaxed);
    }

    void release() {
        if (refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

}  // namespace EntropyEngine::Core
