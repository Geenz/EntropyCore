#include <catch2/catch_test_macros.hpp>
#include "Core/EntropyObject.h"
#include "Core/RefObject.h"
#include <thread>
#include <vector>

using namespace EntropyEngine::Core;

class TestObject : public EntropyObject
{
    ENTROPY_CLASS(TestObject)
    
public:
    int value;
    explicit TestObject(int v = 0) : value(v) {}
    
    int };

class DerivedTestObject : public TestObject
{
    ENTROPY_CLASS(DerivedTestObject)
    
public:
    float extra;
    DerivedTestObject(int v = 0, float e = 0.0f) 
        : TestObject(v), extra(e) {}
    
    float };

TEST_CASE("EntropyObject Basic Functionality", "[EntropyObject]")
{
    SECTION("Initial reference count is 1")
    {
        auto* obj = new TestObject();
        REQUIRE(obj->refCount() == 1);
        obj->release();
    }
    
    SECTION("Retain increases reference count")
    {
        auto* obj = new TestObject();
        obj->retain();
        REQUIRE(obj->refCount() == 2);
        obj->release();
        REQUIRE(obj->refCount() == 1);
        obj->release();
    }
    
    SECTION("Class metadata functions work")
    {
        auto* obj = new TestObject();
        REQUIRE(std::string(obj->className()) == "TestObject");
        REQUIRE(obj->classHash() != 0);
        REQUIRE(obj->toString().find("TestObject") != std::string::npos);
        REQUIRE(obj->description().find("TestObject") != std::string::npos);
        obj->release();
    }
}

TEST_CASE("RefObject Basic Functionality", "[RefObject]")
{
    SECTION("Default constructor creates null RefObject")
    {
        RefObject<TestObject> ref;
        REQUIRE(!ref);
        REQUIRE(ref.get() == nullptr);
    }
    
    SECTION("Explicit constructor takes ownership")
    {
        auto* rawPtr = new TestObject(42);
        RefObject<TestObject> ref(rawPtr);
        REQUIRE(ref);
        REQUIRE(ref.get() == rawPtr);
        REQUIRE(ref->value == 42);
        REQUIRE(rawPtr->refCount() == 1);
    }
    
    SECTION("Copy constructor increments reference count")
    {
        RefObject<TestObject> ref1(new TestObject(42));
        REQUIRE(ref1->refCount() == 1);
        
        RefObject<TestObject> ref2(ref1);
        REQUIRE(ref1->refCount() == 2);
        REQUIRE(ref2->refCount() == 2);
        REQUIRE(ref1.get() == ref2.get());
    }
    
    SECTION("Move constructor transfers ownership")
    {
        RefObject<TestObject> ref1(new TestObject(42));
        auto* ptr = ref1.get();
        
        RefObject<TestObject> ref2(std::move(ref1));
        REQUIRE(!ref1);
        REQUIRE(ref2.get() == ptr);
        REQUIRE(ref2->refCount() == 1);
    }
    
    SECTION("Copy assignment works correctly")
    {
        RefObject<TestObject> ref1(new TestObject(42));
        RefObject<TestObject> ref2(new TestObject(24));
        
        auto* ptr1 = ref1.get();
        auto* ptr2 = ref2.get();
        
        REQUIRE(ptr1->refCount() == 1);
        REQUIRE(ptr2->refCount() == 1);
        
        ref2 = ref1;
        REQUIRE(ref1.get() == ref2.get());
        REQUIRE(ptr1->refCount() == 2);
    }
    
    SECTION("Move assignment works correctly")
    {
        RefObject<TestObject> ref1(new TestObject(42));
        RefObject<TestObject> ref2(new TestObject(24));
        
        auto* ptr1 = ref1.get();
        
        ref2 = std::move(ref1);
        REQUIRE(!ref1);
        REQUIRE(ref2.get() == ptr1);
        REQUIRE(ptr1->refCount() == 1);
    }
    
    SECTION("Operator-> and operator* work correctly")
    {
        RefObject<TestObject> ref(new TestObject(42));
        REQUIRE(ref->value == 42);
        REQUIRE((*ref).value == 42);
        
        ref->value = 100;
        REQUIRE(ref->value == 100);
    }
    
    SECTION("reset() replaces managed object")
    {
        RefObject<TestObject> ref(new TestObject(42));
        auto* newObj = new TestObject(100);
        
        ref.reset(newObj);
        REQUIRE(ref.get() == newObj);
        REQUIRE(ref->value == 100);
        
        ref.reset();
        REQUIRE(!ref);
    }
    
    SECTION("detach() releases ownership")
    {
        RefObject<TestObject> ref(new TestObject(42));
        auto* ptr = ref.detach();
        
        REQUIRE(!ref);
        REQUIRE(ptr != nullptr);
        REQUIRE(ptr->value == 42);
        REQUIRE(ptr->refCount() == 1);
        
        ptr->release();
    }
    
    SECTION("Comparison operators work")
    {
        RefObject<TestObject> ref1(new TestObject(42));
        RefObject<TestObject> ref2(ref1);
        RefObject<TestObject> ref3(new TestObject(42));
        
        REQUIRE(ref1 == ref2);
        REQUIRE(!(ref1 != ref2));
        REQUIRE(ref1 != ref3);
        REQUIRE(!(ref1 == ref3));
    }
}

TEST_CASE("RefObject with inheritance", "[RefObject]")
{
    SECTION("Can convert derived to base")
    {
        RefObject<DerivedTestObject> derived(new DerivedTestObject(42, 3.14f));
        RefObject<TestObject> base(derived);
        
        REQUIRE(base.get() == derived.get());
        REQUIRE(base->value == 42);
        REQUIRE(base->refCount() == 2);
    }
    
    SECTION("Move conversion from derived to base")
    {
        RefObject<DerivedTestObject> derived(new DerivedTestObject(42, 3.14f));
        auto* ptr = derived.get();
        
        RefObject<TestObject> base(std::move(derived));
        REQUIRE(!derived);
        REQUIRE(base.get() == ptr);
        REQUIRE(base->refCount() == 1);
    }
}

TEST_CASE("makeRef helper function", "[RefObject]")
{
    SECTION("Creates object with makeRef")
    {
        auto ref = makeRef<TestObject>(42);
        REQUIRE(ref);
        REQUIRE(ref->value == 42);
        REQUIRE(ref->refCount() == 1);
    }
    
    SECTION("Creates derived object with makeRef")
    {
        auto ref = makeRef<DerivedTestObject>(42, 3.14f);
        REQUIRE(ref);
        REQUIRE(ref->value == 42);
        REQUIRE(ref->extra == 3.14f);
        REQUIRE(ref->refCount() == 1);
    }
}

TEST_CASE("Thread safety", "[EntropyObject][Threading]")
{
    SECTION("Concurrent retain and release")
    {
        auto* obj = new TestObject();
        const int numThreads = 10;
        const int numOps = 1000;
        
        obj->retain();
        
        std::vector<std::thread> threads;
        for (int i = 0; i < numThreads; ++i)
        {
            threads.emplace_back([obj, numOps]()
            {
                for (int j = 0; j < numOps; ++j)
                {
                    obj->retain();
                    std::this_thread::yield();
                    obj->release();
                }
            });
        }
        
        for (auto& thread : threads)
        {
            thread.join();
        }
        
        REQUIRE(obj->refCount() == 2);
        obj->release();
        obj->release();
    }
    
    SECTION("RefObject thread safety")
    {
        RefObject<TestObject> ref(new TestObject(42));
        const int numThreads = 10;
        const int numCopies = 100;
        
        std::vector<std::thread> threads;
        for (int i = 0; i < numThreads; ++i)
        {
            threads.emplace_back([ref, numCopies]()
            {
                for (int j = 0; j < numCopies; ++j)
                {
                    RefObject<TestObject> localCopy = ref;
                    REQUIRE(localCopy->value == 42);
                    std::this_thread::yield();
                }
            });
        }
        
        for (auto& thread : threads)
        {
            thread.join();
        }
        
        REQUIRE(ref->refCount() == 1);
    }
}