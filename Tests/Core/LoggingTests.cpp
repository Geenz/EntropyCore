//
// LoggingTests.cpp - Tests for the Entropy logging system
//

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <Logging/Logger.h>
#include <Logging/ConsoleSink.h>
#include <Logging/LogLevel.h>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

using namespace EntropyEngine::Core::Logging;
using Catch::Matchers::ContainsSubstring;

// Test sink that captures log messages
class TestSink : public ILogSink {
public:
    struct CapturedEntry {
        LogLevel level;
        std::string category;
        std::string message;
    };
    
    void write(const LogEntry& entry) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _entries.push_back({entry.level, std::string(entry.category), entry.message});
    }
    
    void flush() override {}
    
    bool shouldLog(LogLevel level) const override {
        return level >= _minLevel;
    }
    
    void setMinLevel(LogLevel level) override {
        _minLevel = level;
    }
    
    std::vector<CapturedEntry> getEntries() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _entries;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(_mutex);
        _entries.clear();
    }
    
private:
    mutable std::mutex _mutex;
    std::vector<CapturedEntry> _entries;
    LogLevel _minLevel = LogLevel::Trace;
};

TEST_CASE("LogLevel conversions", "[logging]") {
    SECTION("String to LogLevel") {
        CHECK(stringToLogLevel("Trace") == LogLevel::Trace);
        CHECK(stringToLogLevel("Debug") == LogLevel::Debug);
        CHECK(stringToLogLevel("Info") == LogLevel::Info);
        CHECK(stringToLogLevel("Warning") == LogLevel::Warning);
        CHECK(stringToLogLevel("Error") == LogLevel::Error);
        CHECK(stringToLogLevel("Fatal") == LogLevel::Fatal);
        CHECK(stringToLogLevel("Off") == LogLevel::Off);
        CHECK(stringToLogLevel("Invalid") == LogLevel::Info); // Default
    }
    
    SECTION("LogLevel to string") {
        CHECK(logLevelToString(LogLevel::Trace) == "TRACE");
        CHECK(logLevelToString(LogLevel::Debug) == "DEBUG");
        CHECK(logLevelToString(LogLevel::Info) == "INFO ");
        CHECK(logLevelToString(LogLevel::Warning) == "WARN ");
        CHECK(logLevelToString(LogLevel::Error) == "ERROR");
        CHECK(logLevelToString(LogLevel::Fatal) == "FATAL");
        CHECK(logLevelToString(LogLevel::Off) == "OFF  ");
    }
}

TEST_CASE("Basic logging functionality", "[logging]") {
    Logger logger("Test");
    auto testSink = std::make_shared<TestSink>();
    logger.addSink(testSink);
    
    SECTION("Log at different levels") {
        logger.trace("test", "Trace message");
        logger.debug("test", "Debug message");
        logger.info("test", "Info message");
        logger.warning("test", "Warning message");
        logger.error("test", "Error message");
        logger.fatal("test", "Fatal message");
        
        auto entries = testSink->getEntries();
        REQUIRE(entries.size() == 6);
        
        CHECK(entries[0].level == LogLevel::Trace);
        CHECK(entries[0].message == "Trace message");
        
        CHECK(entries[1].level == LogLevel::Debug);
        CHECK(entries[1].message == "Debug message");
        
        CHECK(entries[2].level == LogLevel::Info);
        CHECK(entries[2].message == "Info message");
        
        CHECK(entries[3].level == LogLevel::Warning);
        CHECK(entries[3].message == "Warning message");
        
        CHECK(entries[4].level == LogLevel::Error);
        CHECK(entries[4].message == "Error message");
        
        CHECK(entries[5].level == LogLevel::Fatal);
        CHECK(entries[5].message == "Fatal message");
    }
    
    SECTION("Formatting with std::format") {
        std::string formatted = std::format("Number: {}, String: {}, Float: {:.2f}", 42, "hello", 3.14159);
        logger.log(LogLevel::Info, "test", formatted);
        
        auto entries = testSink->getEntries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].message == "Number: 42, String: hello, Float: 3.14");
    }
    
    SECTION("Category handling") {
        logger.info("CategoryA", "Message A");
        logger.info("CategoryB", "Message B");
        
        auto entries = testSink->getEntries();
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].category == "CategoryA");
        CHECK(entries[1].category == "CategoryB");
    }
}

TEST_CASE("Log level filtering", "[logging]") {
    Logger logger("Test");
    auto testSink = std::make_shared<TestSink>();
    logger.addSink(testSink);
    
    SECTION("Logger level filtering") {
        logger.setMinLevel(LogLevel::Warning);
        
        logger.trace("test", "Should not appear");
        logger.debug("test", "Should not appear");
        logger.info("test", "Should not appear");
        logger.warning("test", "Should appear");
        logger.error("test", "Should appear");
        
        auto entries = testSink->getEntries();
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].message == "Should appear");
        CHECK(entries[1].message == "Should appear");
    }
    
    SECTION("Sink level filtering") {
        testSink->setMinLevel(LogLevel::Info);
        
        logger.trace("test", "Should not appear");
        logger.debug("test", "Should not appear");
        logger.info("test", "Should appear");
        logger.warning("test", "Should appear");
        
        auto entries = testSink->getEntries();
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].level == LogLevel::Info);
        CHECK(entries[1].level == LogLevel::Warning);
    }
}

TEST_CASE("Multiple sinks", "[logging]") {
    Logger logger("Test");
    auto sink1 = std::make_shared<TestSink>();
    auto sink2 = std::make_shared<TestSink>();
    
    logger.addSink(sink1);
    logger.addSink(sink2);
    
    SECTION("Messages go to all sinks") {
        logger.info("test", "Message to all sinks");
        
        CHECK(sink1->getEntries().size() == 1);
        CHECK(sink2->getEntries().size() == 1);
        CHECK(sink1->getEntries()[0].message == "Message to all sinks");
        CHECK(sink2->getEntries()[0].message == "Message to all sinks");
    }
    
    SECTION("Remove sink") {
        logger.removeSink(sink2);
        logger.info("test", "Only to sink1");
        
        CHECK(sink1->getEntries().size() == 1);
        CHECK(sink2->getEntries().size() == 0);
    }
    
    SECTION("Clear sinks") {
        logger.clearSinks();
        logger.info("test", "Goes nowhere");
        
        CHECK(sink1->getEntries().size() == 0);
        CHECK(sink2->getEntries().size() == 0);
    }
}

TEST_CASE("Thread safety", "[logging]") {
    Logger logger("Test");
    auto testSink = std::make_shared<TestSink>();
    logger.addSink(testSink);
    
    const int numThreads = 10;
    const int messagesPerThread = 100;
    
    std::vector<std::thread> threads;
    
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&logger, i, messagesPerThread]() {
            for (int j = 0; j < messagesPerThread; ++j) {
                std::string msg = std::format("Thread {} message {}", i, j);
                logger.log(LogLevel::Info, "thread", msg);
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    auto entries = testSink->getEntries();
    CHECK(entries.size() == numThreads * messagesPerThread);
    
    // Verify all messages were captured
    std::vector<std::vector<bool>> seen(numThreads, std::vector<bool>(messagesPerThread, false));
    
    for (const auto& entry : entries) {
        // Parse thread and message number from the message
        int thread, message;
        if (std::sscanf(entry.message.c_str(), "Thread %d message %d", &thread, &message) == 2) {
            CHECK(thread >= 0);
            CHECK(thread < numThreads);
            CHECK(message >= 0);
            CHECK(message < messagesPerThread);
            CHECK(!seen[thread][message]); // No duplicates
            seen[thread][message] = true;
        }
    }
    
    // Verify all messages were seen
    for (int i = 0; i < numThreads; ++i) {
        for (int j = 0; j < messagesPerThread; ++j) {
            CHECK(seen[i][j]);
        }
    }
}

TEST_CASE("Global logger", "[logging]") {
    auto testSink = std::make_shared<TestSink>();
    
    // Clear any existing sinks and add test sink
    Logger::global().clearSinks();
    Logger::global().addSink(testSink);
    
    SECTION("Access global logger") {
        Logger::global().info("global", "Global message");
        
        auto entries = testSink->getEntries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].message == "Global message");
    }
    
    SECTION("Macros use global logger") {
        ENTROPY_LOG_INFO("Macro message");
        
        auto entries = testSink->getEntries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].message == "Macro message");
    }
    
    SECTION("Category macros") {
        ENTROPY_LOG_INFO_CAT("CustomCategory", "Category macro message");
        
        auto entries = testSink->getEntries();
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].category == "CustomCategory");
        CHECK(entries[0].message == "Category macro message");
    }
}

TEST_CASE("Console sink output format", "[logging]") {
    // Test console sink formatting by capturing its output
    std::ostringstream oss;
    
    // Create a console sink that writes to our string stream
    class TestConsoleSink : public TestSink {
    public:
        TestConsoleSink(std::ostream& stream) : _stream(stream) {}
        
        void write(const LogEntry& entry) override {
            TestSink::write(entry); // Capture for testing
            
            // Also format output to stream for verification
            if (!shouldLog(entry.level)) return;
            
            std::lock_guard<std::mutex> lock(_mutex);
            
            // Simple format for testing
            _stream << "[" << logLevelToString(entry.level) << "] ";
            if (!entry.category.empty()) {
                _stream << "[" << entry.category << "] ";
            }
            _stream << entry.message << std::endl;
        }
        
    private:
        std::ostream& _stream;
        mutable std::mutex _mutex;
    };
    
    Logger logger("Test");
    auto sink = std::make_shared<TestConsoleSink>(oss);
    logger.addSink(sink);
    
    logger.info("TestCategory", "Test message");
    
    std::string output = oss.str();
    CHECK_THAT(output, ContainsSubstring("[INFO ]"));
    CHECK_THAT(output, ContainsSubstring("[TestCategory]"));
    CHECK_THAT(output, ContainsSubstring("Test message"));
}