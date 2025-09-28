#include "EntropyCore.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

int main() {
    // Start the work service
    WorkService::Config cfg{};
    WorkService service(cfg);
    service.start();

    // Create a contract group and register it to the service
    WorkContractGroup group(256, "VFSGroup");
    service.addWorkContractGroup(&group);

    // Create VFS instance with custom config
    VirtualFileSystem::Config vfsConfig;
    vfsConfig.maxWriteLocksCached = 100;  // Smaller cache for testing
    vfsConfig.writeLockTimeout = std::chrono::minutes(1);
    VirtualFileSystem vfs(&group, vfsConfig);

    // Create a file handle for create/delete demo
    auto tmp = vfs.createFileHandle("vfs_create_delete_tmp.txt");
    auto c = tmp.createEmpty();
    c.wait();
    if (c.status() != FileOpStatus::Complete) {
        std::cerr << "createEmpty failed\n";
        return 1;
    }
    auto wtmp = tmp.writeAll("Just to delete me.\n");
    wtmp.wait();
    auto d = tmp.remove();
    d.wait();
    if (d.status() != FileOpStatus::Complete) {
        std::cerr << "remove failed\n";
        return 1;
    }

    // Create a file handle for main demo
    auto handle = vfs.createFileHandle("vfs_example_tmp.txt");

    // Write text
    auto w = handle.writeAll("Hello from VFS!\nLine two.");
    w.wait();
    if (w.status() != FileOpStatus::Complete) {
        std::cerr << "writeAll failed\n";
        return 1;
    }

    // Read all
    auto rAll = handle.readAll();
    rAll.wait();
    if (rAll.status() == FileOpStatus::Complete) {
        std::cout << "readAll (text):\n" << rAll.contentsText() << "\n";
    } else {
        std::cerr << "readAll failed: " << rAll.errorInfo().message << "\n";
    }

    // Read range
    auto rRange = handle.readRange(0, 5);
    rRange.wait();
    if (rRange.status() == FileOpStatus::Complete || rRange.status() == FileOpStatus::Partial) {
        auto bytes = rRange.contentsBytes();
        std::string s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::cout << "readRange(0,5): '" << s << "'\n";
    }

    // Write range (append-like by offset)
    const char* tail = "\nAppended via writeRange.";
    auto wr = handle.writeRange( rAll.contentsBytes().size(), std::span<const std::byte>(reinterpret_cast<const std::byte*>(tail), strlen(tail)) );
    wr.wait();
    if (wr.status() != FileOpStatus::Complete) {
        std::cerr << "writeRange failed\n";
    }

    // Read line 1 (0-based)
    auto rLine = handle.readLine(1);
    rLine.wait();
    if (rLine.status() == FileOpStatus::Complete) {
        std::cout << "readLine(1): '" << rLine.contentsText() << "'\n";
    }
    
    // Test writeLine with atomic rename (improved performance)
    auto wLine = handle.writeLine(1, "Modified line two via atomic rename");
    wLine.wait();
    if (wLine.status() == FileOpStatus::Complete) {
        std::cout << "writeLine succeeded with atomic rename\n";
    } else {
        std::cerr << "writeLine failed: " << wLine.errorInfo().message << "\n";
    }
    
    // Test streaming API for large files
    std::cout << "\nTesting streaming API:\n";
    auto stream = handle.openReadWriteStream();
    if (stream && stream->good()) {
        // Write some data using stream
        const char* streamData = "\nData written via streaming API";
        stream->seek(0, std::ios::end);
        auto writeResult = stream->write(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(streamData), strlen(streamData)));
        stream->flush();
        std::cout << "Wrote " << writeResult.bytesTransferred << " bytes via stream\n";
        
        // Read back using stream
        stream->seek(0, std::ios::beg);
        std::vector<std::byte> buffer(256);
        auto readResult = stream->read(buffer);
        std::cout << "Read " << readResult.bytesTransferred << " bytes via stream\n";
    }
    
    // Test multiple concurrent operations to verify thread safety
    std::cout << "\nTesting concurrent operations:\n";
    std::vector<FileOperationHandle> concurrentOps;
    for (int i = 0; i < 5; ++i) {
        auto h = vfs.createFileHandle("concurrent_test_" + std::to_string(i) + ".txt");
        concurrentOps.push_back(h.writeAll("Concurrent write " + std::to_string(i)));
    }
    
    // Wait for all concurrent writes
    for (auto& op : concurrentOps) {
        op.wait();
        if (op.status() != FileOpStatus::Complete) {
            std::cerr << "Concurrent write failed: " << op.errorInfo().message << "\n";
        }
    }
    std::cout << "All concurrent operations completed\n";
    
    // Clean up test files
    for (int i = 0; i < 5; ++i) {
        auto h = vfs.createFileHandle("concurrent_test_" + std::to_string(i) + ".txt");
        h.remove().wait();
    }

    // Clean up service
    service.stop();
    return 0;
}
