---
title: Virtual File System
---

# Feature: Virtual File System (VFS)

The VFS provides a unified abstraction for file I/O, allowing you to mount different storage backends (Local Disk, Memory, Network) to virtual paths.

## Key Concepts

*   **Virtual Paths**: All file access uses virtual paths (e.g., `Assets/Models/hero.gltf`).
*   **Mount Points**: Mapping virtual prefixes to concrete backends.
*   **Backends**: Implementations of storage logic (`LocalFileSystemBackend`, `MemoryFileSystemBackend`).
*   **FileHandle**: RAII wrapper for opening, reading, and writing files.

## Usage

### 1. Initialization
The VFS requires a `WorkContractGroup` for async execution and a configuration struct.

```cpp
#include <EntropyCore/VirtualFileSystem/VirtualFileSystem.h>
#include <EntropyCore/VirtualFileSystem/LocalFileSystemBackend.h>

// 1. Configure
EntropyEngine::Core::IO::VirtualFileSystem::Config config;
config.serializeWritesPerPath = true;
config.writeLockTimeout = std::chrono::minutes(5);

// 2. Initialize with a shared WorkContractGroup
auto vfs = std::make_unique<VirtualFileSystem>(&ioWorkGroup, config);

// 3. Set Default Backend (Root)
auto localBackend = makeRef<LocalFileSystemBackend>();
vfs->setDefaultBackend(localBackend);

// 4. Mount specific paths
vfs->mountBackend("Network", makeRef<NetworkBackend>());
```

### 2. Reading a File
Use `vfs.open()` (`createFileHandle`) or shortcuts.

```cpp
auto handle = vfs->handle("Assets/Config/app.ini");
if (handle.isValid()) {
    std::string content = handle.readAll();
}
```


### 3. Asynchronous I/O
The VFS integrates with the Work Contract system to perform non-blocking I/O.

```cpp
// Request an async read
vfs->readAsync("Assets/Textures/sky.png", [](const Buffer& data) {
    // Callback runs when data is ready
    loadTexture(data);
});
```

## Write Locking
The VFS supports advisory write locking to prevent data corruption from concurrent writes.

*   **Configurable**: Enable/disable per mount or globally via `Config`.
*   **Timeout**: Locks automatically expire if the writer crashes.
