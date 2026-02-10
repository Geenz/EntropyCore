---
title: Logging
---

# Logging System

EntropyCore features a comprehensive, thread-safe logging system built on C++20 `std::format`.

## Key Concepts

*  ### `ConsoleSink`
Writes to `stdout`/`stderr`. Default for development.

### `FileSink`
Writes to rotated log files. Essential for post-crash analysis in production apps.g., "Database", "Network").
*   **Log Levels**: Severity filtering (Trace, Debug, Info, Warning, Error, Fatal).
*   **Source Location**: Automatic capture of file/line info.

## Usage

### Basic Logging
Use the `ENTROPY_LOG_*` macros. Note that they expect a single string argument; use `std::format` for formatting.

```cpp
#include <EntropyCore/Logging/Logger.h>
#include <format>

void myFunction() {
   ENTROPY_LOG_INFO(std::format("Processing item {}", 42)); // Category: "myFunction"
   ENTROPY_LOG_ERROR(std::format("Failed to load: {}", filename));
}
```

### Categorized Logging
Use `_CAT` macros to specify a subsystem category.

```cpp
ENTROPY_LOG_INFO_CAT("Network", std::format("Packet received from {}", ipAddress));
```

### Configuration
You typically set up sinks in your application entry point.

```cpp
#include <EntropyCore/Logging/ConsoleSink.h>

void setupLogging() {
    auto& logger = EntropyEngine::Core::Logging::Logger::global();

    // Add console output
    logger.addSink(std::make_shared<EntropyEngine::Core::Logging::ConsoleSink>());

    // Set global level
    logger.setMinLevel(EntropyEngine::Core::Logging::LogLevel::Trace);
}
```

### Categories
You can enable/disable categories at runtime to filter noise.

```cpp
Logger::global().disableCategory("Network"); // Mute network spam
```
