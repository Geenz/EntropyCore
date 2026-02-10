---
title: Concurrency
sidebar:
  order: 1
---

# Concurrency

The Concurrency model in Entropy is built around the concept of **Work Contracts** and **Work Graphs**.

## Key Concepts

*   **[Work Contracts](./WorkContracts.md)**: A lock-free mechanism for producer-consumer signaling.
*   **[Work Graphs](./WorkGraphs.md)**: Defining dependencies between tasks to allow parallel execution.
*   **[Work Service](./WorkService.md)**: The runtime that executes the graph.
