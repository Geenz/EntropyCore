---
title: Concurrency
---

# Concurrency System

EntropyCore provides a high-performance, lock-free concurrency system based on **Work Contracts** and **Distributed Contention**.

## Key Concepts

*   **Work Contracts**: Light-weight, atomic units of work.
*   **Work Groups**: Isolated pools of work contracts (e.g., Physics, Rendering).
*   **Work Service**: The execution engine that manages worker threads and scheduling.
*   **Lock-Free**: Designed to scale with core count without mutex contention.

## Architecture

For a deep dive into how the system works, including the Signal Tree and Scheduling logic, please see the Architecture documentation:

*   [**Work Contracts**](/architecture/entropycore/concurrency/workcontracts/): The atomic unit and lock-free mechanism.
*   [**Work Graphs**](/architecture/entropycore/concurrency/workgraphs/): Managing dependencies and DAGs.
*   [**Work Service**](/architecture/entropycore/concurrency/workservice/): The scheduler and engine.

## Quick Usage

```cpp
// 1. Get the WorkService
auto& app = EntropyApplication::shared();
auto workService = app.services().get<WorkService>();

// 2. Define a group
WorkContractGroup physicsGroup(4096, "Physics");

// 3. Register with service
workService->addWorkContractGroup(&physicsGroup);

// 4. Create work
auto handle = physicsGroup.createContract([]() {
    performPhysicsStep();
});

// 5. Schedule
handle.schedule();
```
