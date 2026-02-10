---
title: Debug Utilities
---

# Debug Utilities

EntropyCore includes a suite of tools for debugging and validation, available in `EntropyCore/Debug/DebugUtilities.h`.

## Asserts
Assertions that are stripped from Release builds.

```cpp
// Breaks into debugger if condition fails
ENTROPY_DEBUG_ASSERT(ptr != nullptr, "Pointer cannot be null");
```

## Scoped Helpers

### `DebugScope`
Logs entry and exit of a scope. useful for tracing execution flow.

```cpp
void complexFunction() {
    DebugScope scope("ComplexAlgorithm");
    // Logs: "Entering: ComplexAlgorithm"
    // ...
    // Logs: "Leaving: ComplexAlgorithm" (on exit)
}
```

### `ScopedTimer`
Measures execution time of a block.

```cpp
{
    ScopedTimer timer("DataProcessing");
    // ... heavy work ...
}
// Logs: "DataProcessing took 4.2ms"
```

## Debugger interaction

*   `isDebuggerAttached()`: Returns true if a debugger is present.
*   `debugBreak()`: Triggers a breakpoint programmatically.
