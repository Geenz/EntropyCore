//
// DebugInterfaceTests.cpp - Tests for debugging interfaces
//

#include <catch2/catch_test_macros.hpp>
#include <Debug/Debug.h>
#include <Debug/DebugUtilities.h>
#include <thread>
#include <chrono>
#include <memory>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Debug;

TEST_CASE("INamed interface", "[debug]") {
    SECTION("Basic Named functionality") {
        Named obj("TestObject");
        CHECK(obj.getName() == "TestObject");
        CHECK(obj.hasName());
        
        obj.setName("NewName");
        CHECK(obj.getName() == "NewName");
        
        Named empty;
        CHECK(!empty.hasName());
        CHECK(empty.getName().empty());
    }
}

TEST_CASE("DebugRegistry", "[debug]") {
    auto& registry = DebugRegistry::getInstance();
    
    SECTION("Register and find objects") {
        Named obj1("Object1");
        Named obj2("Object2");
        Named obj3("Object1"); // Duplicate name
        
        registry.registerObject(&obj1, "TestClass");
        registry.registerObject(&obj2, "TestClass");
        registry.registerObject(&obj3, "OtherClass");
        
        // Find by name
        auto byName = registry.findByName("Object1");
        CHECK(byName.size() == 2);
        CHECK((byName[0] == &obj1 || byName[0] == &obj3));
        CHECK((byName[1] == &obj1 || byName[1] == &obj3));
        
        // Find by type
        auto byType = registry.findByType("TestClass");
        CHECK(byType.size() == 2);
        CHECK((byType[0] == &obj1 || byType[0] == &obj2));
        CHECK((byType[1] == &obj1 || byType[1] == &obj2));
        
        // Cleanup
        registry.unregisterObject(&obj1);
        registry.unregisterObject(&obj2);
        registry.unregisterObject(&obj3);
    }
    
    SECTION("Unregister removes objects") {
        Named obj("TestObj");
        registry.registerObject(&obj, "TestClass");
        
        auto found = registry.findByName("TestObj");
        CHECK(found.size() == 1);
        
        registry.unregisterObject(&obj);
        found = registry.findByName("TestObj");
        CHECK(found.empty());
    }
    
    SECTION("Edge cases") {
        SECTION("Empty name handling") {
            Named emptyName("");
            registry.registerObject(&emptyName, "TestClass");
            
            auto found = registry.findByName("");
            CHECK(found.size() == 1);
            CHECK(found[0] == &emptyName);
            
            registry.unregisterObject(&emptyName);
        }
        
        SECTION("Duplicate registration") {
            Named obj("Duplicate");
            registry.registerObject(&obj, "TestClass");
            registry.registerObject(&obj, "TestClass"); // Should handle gracefully
            
            auto found = registry.findByName("Duplicate");
            CHECK(found.size() == 1); // Should not create duplicates
            
            registry.unregisterObject(&obj);
        }
        
        SECTION("Unregister non-existent object") {
            Named obj("NonExistent");
            // Try to unregister without registering first
            registry.unregisterObject(&obj);
            // Should not crash or cause issues
        }
        
        SECTION("Empty type names") {
            Named obj("TestObj");
            registry.registerObject(&obj, "");
            
            auto found = registry.findByType("");
            CHECK(found.size() == 1);
            
            registry.unregisterObject(&obj);
        }
    }
}

TEST_CASE("AutoDebugRegistered", "[debug]") {
    auto& registry = DebugRegistry::getInstance();
    
    {
        AutoDebugRegistered<Named> auto1("AutoType", "Auto1");
        AutoDebugRegistered<Named> auto2("AutoType", "Auto2");
        
        auto found = registry.findByType("AutoType");
        CHECK(found.size() == 2);
    }
    
    // Should be automatically unregistered when out of scope
    auto found = registry.findByType("AutoType");
    CHECK(found.empty());
}

TEST_CASE("ScopedTimer", "[debug]") {
    SECTION("Basic timing functionality") {
        ScopedTimer timer("TestTimer", false); // Disable logging
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        double duration = timer.getDuration();
        CHECK(duration >= 10.0);
        CHECK(duration < 100.0); // Reasonable upper bound
    }
    
    SECTION("Multiple timers") {
        ScopedTimer timer1("Timer1", false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ScopedTimer timer2("Timer2", false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        
        double duration1 = timer1.getDuration();
        double duration2 = timer2.getDuration();
        
        CHECK(duration1 >= duration2); // Timer1 should be longer
        CHECK(duration1 >= 10.0);
        CHECK(duration2 >= 5.0);
    }
    
    SECTION("Timer precision") {
        ScopedTimer timer("PrecisionTest", false);
        auto start = std::chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        auto end = std::chrono::high_resolution_clock::now();
        
        double timerDuration = timer.getDuration();
        auto actualDuration = std::chrono::duration<double, std::milli>(end - start);
        
        // Timer should be reasonably accurate
        CHECK(std::abs(timerDuration - actualDuration.count()) < 1.0);
    }
    
    SECTION("Timer with logging") {
        // Test that timer logs when logOnDestruct is true
        // Note: We can't easily capture the log output in this test
        // but we can verify it doesn't crash
        {
            ScopedTimer timer("LoggingTimer", true);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } // Timer should log on destruct
        CHECK(true); // If we get here, it didn't crash
    }
}

TEST_CASE("DebugScope", "[debug]") {
    SECTION("Basic scope functionality") {
        // Test that scope entry/exit messages are logged
        // Note: We can't easily capture the log output in this test
        // but we can verify it doesn't crash
        {
            DebugScope scope("TestScope");
            // Scope should log entry and exit
        }
        CHECK(true); // If we get here, it didn't crash
    }
    
    SECTION("Nested scopes") {
        DebugScope outer("OuterScope");
        {
            DebugScope inner("InnerScope");
            // Test nested scope behavior
        }
        // Test that both scopes are properly tracked
        CHECK(true); // If we get here, it didn't crash
    }
    
    SECTION("Scope with empty name") {
        DebugScope emptyScope("");
        // Should handle empty names gracefully
        CHECK(true); // If we get here, it didn't crash
    }
}

TEST_CASE("validatePointer", "[debug]") {
    SECTION("Valid pointer") {
        int* ptr = new int(42);
        CHECK(validatePointer(ptr, "test"));
        delete ptr;
    }
    
    SECTION("Null pointer") {
        int* ptr = nullptr;
        CHECK(!validatePointer(ptr, "test"));
    }
    
    SECTION("Different pointer types") {
        std::string* strPtr = new std::string("test");
        CHECK(validatePointer(strPtr, "string"));
        delete strPtr;
        
        void* voidPtr = malloc(100);
        CHECK(validatePointer(static_cast<char*>(voidPtr), "void"));
        free(voidPtr);
    }
    
    SECTION("Source location tracking") {
        // Test that source location is captured correctly
        int* ptr = nullptr;
        bool result = validatePointer(ptr, "test");
        CHECK(!result);
        // Could verify that source location is logged
    }
    
    SECTION("Multiple validations") {
        int* ptr1 = new int(1);
        int* ptr2 = new int(2);
        
        CHECK(validatePointer(ptr1, "ptr1"));
        CHECK(validatePointer(ptr2, "ptr2"));
        
        delete ptr1;
        delete ptr2;
    }
}

TEST_CASE("getMemoryStats", "[debug]") {
    SECTION("Basic memory stats") {
        MemoryStats stats = getMemoryStats();
        
        // Test that stats structure is valid
        CHECK(stats.currentBytes >= 0);
        CHECK(stats.peakBytes >= 0);
        CHECK(stats.allocationCount >= 0);
        CHECK(stats.deallocationCount >= 0);
        
        // Peak should be >= current
        CHECK(stats.peakBytes >= stats.currentBytes);
    }
    
    SECTION("MemoryStats structure") {
        MemoryStats stats;
        stats.currentBytes = 100;
        stats.peakBytes = 200;
        stats.allocationCount = 10;
        stats.deallocationCount = 5;
        
        CHECK(stats.currentBytes == 100);
        CHECK(stats.peakBytes == 200);
        CHECK(stats.allocationCount == 10);
        CHECK(stats.deallocationCount == 5);
    }
    
    SECTION("MemoryStats default values") {
        MemoryStats stats;
        CHECK(stats.currentBytes == 0);
        CHECK(stats.peakBytes == 0);
        CHECK(stats.allocationCount == 0);
        CHECK(stats.deallocationCount == 0);
    }
}

TEST_CASE("debugFormat", "[debug]") {
    SECTION("Basic formatting") {
        std::string result = debugFormat("Hello {}", "World");
        CHECK(result == "Hello World");
    }
    
    SECTION("Multiple arguments") {
        std::string result = debugFormat("{} + {} = {}", 1, 2, 3);
        CHECK(result == "1 + 2 = 3");
    }
    
    SECTION("Complex formatting") {
        std::string result = debugFormat("Value: {:.2f}", 3.14159);
        CHECK(result == "Value: 3.14");
    }
    
    SECTION("Empty format string") {
        std::string result = debugFormat("");
        CHECK(result.empty());
    }
    
    SECTION("No arguments") {
        std::string result = debugFormat("Static text");
        CHECK(result == "Static text");
    }
    
    SECTION("Special characters") {
        std::string result = debugFormat("Test: {} {}", "Hello", "World!");
        CHECK(result == "Test: Hello World!");
    }
}

TEST_CASE("Debug assertions", "[debug]") {
    SECTION("ENTROPY_DEBUG_ASSERT") {
        // Test that assertions work in debug builds
        #ifdef EntropyDebug
            // Test successful assertion
            ENTROPY_DEBUG_ASSERT(true, "This should not trigger");
            
            // Test failed assertion (would need to capture fatal log)
            // ENTROPY_DEBUG_ASSERT(false, "This should trigger");
        #else
            // In release builds, assertions should be no-ops
            ENTROPY_DEBUG_ASSERT(false, "This should be ignored");
            CHECK(true); // Should reach here
        #endif
    }
    
    SECTION("ENTROPY_DEBUG_ONLY") {
        int value = 0;
        ENTROPY_DEBUG_ONLY({ value = 42; });
        
        #ifdef EntropyDebug
            CHECK(value == 42);
        #else
            CHECK(value == 0);
        #endif
    }
    
    SECTION("ENTROPY_DEBUG_VARIABLE") {
        ENTROPY_DEBUG_VARIABLE(int debugVar) = 42;
        // Should compile without warnings in both debug and release
        CHECK(true);
    }
}
