# EntropyCore

Core utilities and concurrency building blocks for the Entropy Engine. This library provides a type-safe handle system, a fast DAG implementation, structured debugging/profiling hooks, and a modern, lock-free work scheduling system with an optional thread-pool service.

## Features

- Type-safe handles with generation-based validation
- Lock-free Work Contract system for fine-grained parallelism
- WorkService thread pool with pluggable schedulers (Adaptive, Round-robin, Random, Direct)
- Directed Acyclic Graph (DAG) utilities for dependency management
- Lightweight logging with pluggable sinks (Console, etc.)
- Debug/profiling helpers with Tracy integration

## Components

- Core: common headers and base types
- TypeSystem: handle and reflection utilities
- Concurrency: WorkContract, WorkGraph, WorkService and schedulers
- Graph: DAG data structures and helpers
- Debug: debug interfaces, object registry, profiling macros
- Logging: logger and sinks

## Requirements

- CMake 3.28+
- C++20 compiler
  - Windows: MSVC (Visual Studio 2022 or newer)
  - macOS: AppleClang 15+
  - Linux: recent Clang/GCC
- vcpkg (manifest mode). Dependencies are declared in vcpkg.json and configured via vcpkg-configuration.json.

## Build

Basic CMake workflow (manifest mode vcpkg):

```bash
# Configure
cmake -B build -S .

# Build the main library (default target) and examples
cmake --build build --target EntropyCore
cmake --build build --target EntropyObjectExample WorkContractExample WorkGraphYieldableExample

# (Optional) run tests if enabled
ctest --test-dir build
```

Notes:
- If your CMake does not auto-detect vcpkg, pass the toolchain:
  - -DCMAKE_TOOLCHAIN_FILE="[VCPKG_ROOT]/scripts/buildsystems/vcpkg.cmake"
- On Windows/MSVC, static CRT is used to match vcpkg defaults.

### Building tests

Tests are off by default. Enable and build:

```bash
cmake -B build -S . -DENTROPY_BUILD_TESTS=ON
cmake --build build --target EntropyCoreTests
ctest --test-dir build --output-on-failure
```

## Using in your project

You can consume EntropyCore either by adding this repository to your build, or by installing and using find_package.

1) Add as a subdirectory (no install required):

```cmake
add_subdirectory(path/to/EntropyCore)
# Local target name
target_link_libraries(YourTarget PRIVATE EntropyCore)
```

2) Installed package via find_package:

```cmake
find_package(EntropyCore REQUIRED)
# Exported target name
target_link_libraries(YourTarget PRIVATE EntropyCore::Core)
```

## Quick start

### Work Contract System

The Work Contract system provides a lock-free framework for managing parallel work execution.

- WorkContractGroup: lock-free work pool and scheduling primitives
- WorkContractHandle: EntropyObject-based handle stamped with owner/index/generation

Credit: Inspired by Michael A. Maniscalco’s work_contract (https://github.com/buildingcpp/work_contract).

Basic example: create a group, add work, schedule, then execute in a simple loop:

```cpp
#include <EntropyCore.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <optional>
using namespace EntropyEngine::Core::Concurrency;

int main() {
    WorkContractGroup workGroup(1024, "ExampleGroup");

    auto handle1 = workGroup.createContract([]{
        std::cout << "Task 1\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    auto handle2 = workGroup.createContract([]{
        std::cout << "Task 2\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });

    handle1.schedule();
    handle2.schedule();

    while (true) {
        WorkContractHandle handle = workGroup.selectForExecution(std::nullopt);
        if (!handle.valid()) break;
        workGroup.executeContract(handle);
        workGroup.completeExecution(handle);
    }
    std::cout << "All work completed\n";
}
```

Manual execution loop (if you want a custom executor):

```cpp
while (true) {
    WorkContractHandle handle = workGroup.selectForExecution(std::nullopt);
    if (!handle.valid()) break;
    workGroup.executeContract(handle);
    workGroup.completeExecution(handle);
}
```

### WorkService (thread pool)

WorkService executes work from one or more WorkContractGroups using worker threads and a pluggable scheduling strategy.

```cpp
#include <EntropyCore.h>
#include <iostream>
using namespace EntropyEngine::Core::Concurrency;

int main() {
    WorkService::Config config; config.threadCount = 4;
    WorkService service(config);

    WorkContractGroup group(512);
    service.addWorkContractGroup(&group);

    service.start();

    auto handle = group.createContract([]{ std::cout << "Hello from worker thread!\n"; });
    handle.schedule();

    group.wait();
    service.stop();
}
```

Custom scheduler example:

```cpp
IWorkScheduler::Config schedulerConfig;
auto roundRobinScheduler = std::make_unique<RoundRobinScheduler>(schedulerConfig);
WorkService service(config, std::move(roundRobinScheduler));
```

You can add multiple groups to the same service; all will be executed by the worker pool.

## Examples

This repository builds a few small examples:
- EntropyObjectExample
- WorkContractExample
- WorkGraphYieldableExample

Build them explicitly with CMake’s --target as shown above.

## Contributing

- See CODESTYLE.md for coding style.
- See DOCUMENTATION.md for additional developer docs.
- Please include tests where reasonable (enable with -DENTROPY_BUILD_TESTS=ON).

## License

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed with this file, you can obtain one at https://mozilla.org/MPL/2.0/.

Copyright (c) 2025 Jonathan "Geenz" Goodman