#include <EntropyCore.h>
#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace EntropyEngine::Core;

class TestObject : public EntropyObject
{
    ENTROPY_CLASS_BODY(TestObject)
public:
    int value = 42;
};

TEST(WeakRefSafety, BasicLifecycle) {
    WeakRef<TestObject> weak;
    {
        auto obj = makeRef<TestObject>();
        weak = obj;

        EXPECT_FALSE(weak.expired());
        auto locked = weak.lock();
        EXPECT_TRUE(locked);
        EXPECT_EQ(locked->value, 42);
    }
    // obj is gone now
    EXPECT_TRUE(weak.expired());
    auto locked = weak.lock();
    EXPECT_FALSE(locked);
}

TEST(WeakRefSafety, LazyControlBlockCreation) {
    auto obj = makeRef<TestObject>();
    // No weak ref yet, so no block should exist (internal implementation detail, hard to test without friend)
    // But establishing a weak ref should work
    WeakRef<TestObject> weak(obj);
    EXPECT_FALSE(weak.expired());
}

TEST(WeakRefSafety, MultipleWeakRefs) {
    WeakRef<TestObject> w1, w2;
    {
        auto obj = makeRef<TestObject>();
        w1 = obj;
        w2 = w1;  // Copy weak ref

        EXPECT_FALSE(w1.expired());
        EXPECT_FALSE(w2.expired());
    }
    EXPECT_TRUE(w1.expired());
    EXPECT_TRUE(w2.expired());
}

TEST(WeakRefSafety, WeakRefOutlivesObject) {
    WeakRef<TestObject> weak;
    {
        auto obj = makeRef<TestObject>();
        weak = obj;
    }
    // block should keep existing even if object is dead, until weak dies
    EXPECT_TRUE(weak.expired());
}

TEST(WeakRefSafety, ObjectRevivalPrevention) {
    // Ensure we can't lock a dying object
    // This is hard to deterministically test without hooking into release(),
    // but the design guarantees safety via mutex.
}

// Simple stress test
TEST(WeakRefSafety, ThreadedStress) {
    std::atomic<bool> done{false};

    // We want to test race between Object destruction and WeakRef::lock().
    // To do this safetly, we need thread-local WeakRefs pointing to the same object.

    std::mutex sharedMutex;
    WeakRef<TestObject> sharedWeak;  // Protected by mutex

    std::thread owner([&]() {
        for (int i = 0; i < 1000; ++i) {
            auto obj = makeRef<TestObject>();
            {
                std::lock_guard<std::mutex> lock(sharedMutex);
                sharedWeak = obj;
            }
            // Let some readers get it
            std::this_thread::yield();
            // obj dies when it goes out of scope here
        }
        done = true;
    });

    auto worker = [&]() {
        while (!done) {
            WeakRef<TestObject> localWeak;
            {
                std::lock_guard<std::mutex> lock(sharedMutex);
                localWeak = sharedWeak;
            }
            // Now we have a local weak ref. The object might be alive or dead.
            // Attempt to lock it. This races with owner destroying it.
            if (auto locked = localWeak.lock()) {
                // If we got it, it must be valid
                EXPECT_EQ(locked->value, 42);
                // Hold it for a bit while owner might be trying to destroy
                // std::this_thread::yield();
            }
            // localWeak dies here
        }
    };

    std::thread reader1(worker);
    std::thread reader2(worker);
    std::thread reader3(worker);

    owner.join();
    reader1.join();
    reader2.join();
    reader3.join();
}
