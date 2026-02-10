---
title: Memory Model
---

# Feature: Memory Model

EntropyCore employs a hybrid memory model that combines **intrusive reference counting** with safe **weak references** and optional **shared_ptr interoperability**.

## Core Concepts

### `EntropyObject`
The base class for all managed objects. It provides:
*   **Intrusive Reference Counting**: Thread-safe `refCount` embedded in the object.
*   **Weak Reference Support**: Lazily allocated control block for weak pointers.
*   **Handle Identity**: Optional ownership tracking for handle-based systems.

### `RefObject<T>`
The primary smart pointer for `EntropyObject`.
*   Similar to `std::intrusive_ptr` or `boost::intrusive_ptr`.
*   Zero allocation (pointer size only).
*   Fast (direct atomics, no external control block for strong refs).

```cpp
// Create a ref-counted object
RefObject<Texture> texture = makeRef<Texture>("hero.png");

// Pass by value (increments refcount) or const reference
void render(RefObject<Texture> tex);
```

### `WeakRef<T>`
A non-owning reference that safely detects object destruction.
*   Uses a `WeakControlBlock` managed by the object.
*   Thread-safe `lock()` returns a `RefObject<T>`.

```cpp
WeakRef<Texture> weakTex = texture;

if (auto locked = weakTex.lock()) {
    // Object is alive and guaranteed to stay alive in this scope
    locked->bind();
} else {
    // Object has been destroyed
}
```

## `std::shared_ptr` Interoperability

While `RefObject` is preferred for internal engine use, `std::shared_ptr` is supported for integration with external libraries or legacy code.

### `toSharedPtr`
Converts an `EntropyObject` to a `std::shared_ptr` that shares ownership with the intrusive refcount.

```cpp
#include <EntropyCore/Core/EntropyInterop.h>

RefObject<MyObject> obj = makeRef<MyObject>();

// Create a shared_ptr that increments the INTRUSIVE refcount
std::shared_ptr<MyObject> shared = toSharedPtr(obj);
```

**Note**: The custom key-holder ensures that `std::shared_ptr` calls `release()` instead of `delete`.

### `fromSharedPtr`
Converts a compatible `std::shared_ptr` back to `RefObject`.

```cpp
RefObject<MyObject> back = fromSharedPtr(shared);
```

## Best Practices

1.  **Use `RefObject` by default**: control block is smaller (none), faster (intrusive).
2.  **Use `WeakRef` for cycles**: Parent -> Child (Strong), Child -> Parent (Weak).
3.  **Avoid `std::shared_ptr` inside hot loops**: Extra allocations/indirection unless needed for external APIs.
