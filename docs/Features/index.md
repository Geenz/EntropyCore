---
title: Entropy Core Features
sidebar:
  order: 0
---

EntropyCore is the foundational library for the Entropy framework, providing essential systems for high-performance application development.

## Core Services & Lifecycle

| Feature | Description |
| :--- | :--- |
| [**Core Services**](./coreservices/) | Application lifecycle, Service Registry, Event Bus, and Timers. |
| [**Memory Model**](./memorymodel/) | Hybrid memory management with `RefObject` (intrusive), `WeakRef`, and `std::shared_ptr` interop. |
| [**Type System**](./typesystem/) | Static reflection, `TypeID`, and runtime introspection for serialization and UI generators. |

## System Features

| Feature | Description |
| :--- | :--- |
| [**Concurrency**](./concurrency/) | Task-based parallelism engine (`WorkContracts`) for lock-free, scalable execution. |
| [**Virtual File System**](./virtualfilesystem/) | Unified I/O abstraction supporting mount points, async operations, and multiple backends. |
| [**Logging**](./logging/) | High-performance, categorized logging system based on C++20 `std::format`. |
| [**Debug Utilities**](./debugutils/) | Scoped timers, debug assertions, and visual debugger integration tools. |
