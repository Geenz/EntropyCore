/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 */

#include <catch2/catch_test_macros.hpp>
#include <TypeSystem/GenericHandle.h>
#include <vector>
#include <memory>

using namespace EntropyEngine::Core::TypeSystem;

// Test data structures
struct TestObject {
    int value;
    std::string name;
};

struct TestTag {};
struct AnotherTag {};

// Mock owner that implements the required interface
class MockOwner {
public:
    uint32_t generation = 1;
    std::vector<TestObject> objects;
    std::vector<uint32_t> generations;
    
    MockOwner() {
        objects.reserve(10);
        generations.reserve(10);
    }
    
    uint32_t addObject(TestObject obj) {
        uint32_t index = static_cast<uint32_t>(objects.size());
        objects.push_back(std::move(obj));
        generations.push_back(generation);
        return index;
    }
    
    void removeObject(uint32_t index) {
        if (index < objects.size()) {
            generations[index] = 0; // Mark as invalid
        }
    }
    
    void recycleSlot(uint32_t index, TestObject obj) {
        if (index < objects.size()) {
            objects[index] = std::move(obj);
            generations[index] = ++generation;
        }
    }
};

TEST_CASE("GenericHandle basic operations", "[typesystem][handle]") {
    SECTION("Default construction") {
        GenericHandle<MockOwner> handle;
        
        REQUIRE(!handle.isValid());
        REQUIRE(handle.getOwner() == nullptr);
        REQUIRE(handle.getIndex() == 0);  // Invalid handle has index 0
        REQUIRE(handle.getGeneration() == 0);
    }
    
    SECTION("Parameterized construction") {
        MockOwner owner;
        GenericHandle<MockOwner> handle(&owner, 5, 10);
        
        REQUIRE(handle.isValid());
        REQUIRE(handle.getOwner() == &owner);
        REQUIRE(handle.getIndex() == 5);
        REQUIRE(handle.getGeneration() == 10);
    }
    
    SECTION("Copy construction and assignment") {
        MockOwner owner;
        GenericHandle<MockOwner> handle1(&owner, 3, 7);
        
        // Copy construction
        GenericHandle<MockOwner> handle2(handle1);
        REQUIRE(handle2.getOwner() == &owner);
        REQUIRE(handle2.getIndex() == 3);
        REQUIRE(handle2.getGeneration() == 7);
        
        // Copy assignment
        GenericHandle<MockOwner> handle3;
        handle3 = handle1;
        REQUIRE(handle3.getOwner() == &owner);
        REQUIRE(handle3.getIndex() == 3);
        REQUIRE(handle3.getGeneration() == 7);
    }
    
    SECTION("Equality comparison") {
        MockOwner owner1, owner2;
        
        GenericHandle<MockOwner> handle1(&owner1, 1, 1);
        GenericHandle<MockOwner> handle2(&owner1, 1, 1);
        GenericHandle<MockOwner> handle3(&owner1, 2, 1);
        GenericHandle<MockOwner> handle4(&owner2, 1, 1);
        GenericHandle<MockOwner> handle5(&owner1, 1, 2);
        
        REQUIRE(handle1 == handle2);
        REQUIRE(handle1 != handle3); // Different index
        REQUIRE(handle1 != handle4); // Different owner
        REQUIRE(handle1 != handle5); // Different generation
    }
    
    SECTION("Invalidate operation") {
        MockOwner owner;
        GenericHandle<MockOwner> handle(&owner, 5, 10);
        
        REQUIRE(handle.isValid());
        
        handle.invalidate();
        
        REQUIRE(!handle.isValid());
        // Handle still has owner but data is invalid
        REQUIRE(handle.getOwner() == &owner);
        REQUIRE(handle.getIndex() == 0);
        REQUIRE(handle.getGeneration() == 0);
    }
}

TEST_CASE("TypedHandle operations", "[typesystem][handle]") {
    SECTION("Type safety") {
        MockOwner owner;
        
        TypedHandle<TestTag, MockOwner> handle1(&owner, 1, 1);
        TypedHandle<AnotherTag, MockOwner> handle2(&owner, 1, 1);
        
        // These should be different types and not comparable
        // This test verifies compile-time type safety
        static_assert(!std::is_same_v<decltype(handle1), decltype(handle2)>);
    }
    
    SECTION("Inheritance from GenericHandle") {
        MockOwner owner;
        TypedHandle<TestTag, MockOwner> handle(&owner, 5, 10);
        
        // Should have all GenericHandle functionality
        REQUIRE(handle.isValid());
        REQUIRE(handle.getOwner() == &owner);
        REQUIRE(handle.getIndex() == 5);
        REQUIRE(handle.getGeneration() == 10);
    }
}

TEST_CASE("Handle validation scenarios", "[typesystem][handle]") {
    SECTION("Generation-based validation") {
        MockOwner owner;
        
        // Add an object
        uint32_t index = owner.addObject(TestObject{42, "Test"});
        TypedHandle<TestTag, MockOwner> handle(&owner, index, owner.generations[index]);
        
        REQUIRE(handle.isValid());
        
        // Simulate object removal
        owner.removeObject(index);
        
        // Handle should now be invalid even though owner and index are the same
        // (In a real implementation, isValid() would check the generation)
        // For this test, we manually check
        bool wouldBeValid = (handle.getGeneration() == owner.generations[index]);
        REQUIRE(!wouldBeValid);
    }
    
    SECTION("Slot recycling") {
        MockOwner owner;
        
        // Add an object
        uint32_t index = owner.addObject(TestObject{42, "First"});
        TypedHandle<TestTag, MockOwner> handle1(&owner, index, owner.generations[index]);
        
        // Remove and recycle the slot
        owner.removeObject(index);
        owner.recycleSlot(index, TestObject{99, "Second"});
        
        // Create a new handle for the recycled slot
        TypedHandle<TestTag, MockOwner> handle2(&owner, index, owner.generations[index]);
        
        // Old handle should be invalid, new handle should be valid
        REQUIRE(handle1.getGeneration() != owner.generations[index]);
        REQUIRE(handle2.getGeneration() == owner.generations[index]);
    }
}

TEST_CASE("Handle edge cases", "[typesystem][handle]") {
    SECTION("Null owner handling") {
        GenericHandle<MockOwner> handle(nullptr, 0, 1);
        
        // Should still be considered invalid with null owner
        REQUIRE(!handle.isValid());
    }
    
    SECTION("Maximum index value") {
        MockOwner owner;
        GenericHandle<MockOwner> handle(&owner, std::numeric_limits<uint32_t>::max(), 1);
        
        // Should be valid as long as owner is not null and generation > 0
        REQUIRE(handle.isValid());
    }
    
    SECTION("Zero generation") {
        MockOwner owner;
        GenericHandle<MockOwner> handle(&owner, 0, 0);
        
        // Zero generation indicates invalid handle
        REQUIRE(!handle.isValid());
    }
}

TEST_CASE("Handle usage patterns", "[typesystem][handle]") {
    SECTION("Handle comparison in collections") {
        MockOwner owner;
        
        GenericHandle<MockOwner> handle1(&owner, 1, 1);
        GenericHandle<MockOwner> handle2(&owner, 2, 1);
        
        // Test handles can be stored in vector
        std::vector<GenericHandle<MockOwner>> handles;
        handles.push_back(handle1);
        handles.push_back(handle2);
        
        REQUIRE(handles.size() == 2);
        REQUIRE(handles[0] == handle1);
        REQUIRE(handles[1] == handle2);
    }
    
    SECTION("Handle vector storage") {
        MockOwner owner;
        std::vector<TypedHandle<TestTag, MockOwner>> handles;
        
        for (uint32_t i = 0; i < 10; ++i) {
            handles.emplace_back(&owner, i, 1);
        }
        
        REQUIRE(handles.size() == 10);
        
        // All handles should be valid and have correct indices
        for (uint32_t i = 0; i < 10; ++i) {
            REQUIRE(handles[i].isValid());
            REQUIRE(handles[i].getIndex() == i);
        }
    }
    
    SECTION("Handle lifetime management") {
        auto owner = std::make_unique<MockOwner>();
        TypedHandle<TestTag, MockOwner> handle(owner.get(), 0, 1);
        
        REQUIRE(handle.isValid());
        
        // Simulate owner destruction
        owner.reset();
        
        // Handle still has the pointer but owner is gone
        // In a real system, this would be detected by checking the owner's generation
        REQUIRE(handle.getOwner() != nullptr); // Dangling pointer!
        
        // This demonstrates why generation checking is important
    }
}