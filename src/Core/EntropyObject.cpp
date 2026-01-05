//
// Created by goodh on 9/17/2025.
//

#include "EntropyObject.h"

#include <format>
#include <functional>

#include "../Logging/Logger.h"
#include "../TypeSystem/TypeID.h"
#include "WeakControlBlock.h"

namespace EntropyEngine::Core
{

void EntropyObject::retain() const noexcept {
    _refCount.fetch_add(1, std::memory_order_acq_rel);
}

void EntropyObject::release() const noexcept {
    uint32_t oldCount = _refCount.fetch_sub(1, std::memory_order_acq_rel);

#ifdef ENTROPY_DEBUG
    uint32_t newCount = oldCount - 1;
    ENTROPY_LOG_TRACE_CAT("RefCount", std::format("Release {} @ {} -> refcount={}", className(),
                                                  static_cast<const void*>(this), newCount));
#endif

    if (oldCount == 1) {
        // If we have a weak block, we must safely detach from it
        if (WeakControlBlock* block = _weakBlock.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(block->mutex);
            block->object = nullptr;
            // We are done with our reference to the block
            block->release();
        }

#ifdef ENTROPY_DEBUG
        const char* name = className();
        ENTROPY_LOG_TRACE_CAT("RefCount", std::format("Delete {} @ {}", name, static_cast<const void*>(this)));
#endif
        // Call memory profiling hook before delete (while pointer is still valid)
        if (EntropyObjectMemoryHooks::onFree) {
            EntropyObjectMemoryHooks::onFree(const_cast<void*>(static_cast<const void*>(this)), "EntropyObject");
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        delete this;
    }
}

uint32_t EntropyObject::refCount() const noexcept {
    // Use acquire to ensure we observe the latest completed updates after thread joins
    return _refCount.load(std::memory_order_acquire);
}

bool EntropyObject::tryRetain() const noexcept {
    uint32_t count = _refCount.load(std::memory_order_acquire);
    while (count != 0) {
        if (_refCount.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
        // count has been updated with the current value; loop and retry unless it hit 0
    }
    return false;
}

uint64_t EntropyObject::classHash() const noexcept {
    static const uint64_t hash =
        static_cast<uint64_t>(EntropyEngine::Core::TypeSystem::createTypeId<EntropyObject>().id);
    return hash;
}

std::string EntropyObject::toString() const {
    return std::format("{}@{}", className(), static_cast<const void*>(this));
}

std::string EntropyObject::debugString() const {
    // Avoid repeating className() since toString() already includes it by default
    if (hasHandle()) {
        return std::format("{} [refs:{} handle:{:08X}:{:08X}]", toString(), refCount(), handleIndex(),
                           handleGeneration());
    }
    return std::format("{} [refs:{}]", toString(), refCount());
}

std::string EntropyObject::description() const {
    return toString();
}

WeakControlBlock* EntropyObject::getWeakControlBlock() const {
    WeakControlBlock* block = _weakBlock.load(std::memory_order_acquire);
    if (!block) {
        // Create new block starting with refCount=1 (owned by us)
        WeakControlBlock* newBlock = new WeakControlBlock(const_cast<EntropyObject*>(this));

        WeakControlBlock* expected = nullptr;
        if (_weakBlock.compare_exchange_strong(expected, newBlock, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            block = newBlock;
        } else {
            // Race lost, delete ours and use winner's
            delete newBlock;
            block = expected;
        }
    }
    return block;
}

}  // namespace EntropyEngine::Core
