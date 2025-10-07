#include "EntropyCore.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/WriteBatch.h"
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
    WorkContractGroup group(2000, "VFSGroup");
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
        ENTROPY_LOG_ERROR("createEmpty failed");
        return 1;
    }
    auto wtmp = tmp.writeAll("Just to delete me.\n");
    wtmp.wait();
    auto d = tmp.remove();
    d.wait();
    if (d.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR("remove failed");
        return 1;
    }

    // Create a file handle for main demo
    auto handle = vfs.createFileHandle("vfs_example_tmp.txt");

    // Write text
    auto w = handle.writeAll("Hello from VFS!\nLine two.");
    w.wait();
    if (w.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR("writeAll failed");
        return 1;
    }

    // Read all
    auto rAll = handle.readAll();
    rAll.wait();
    if (rAll.status() == FileOpStatus::Complete) {
        ENTROPY_LOG_INFO("readAll (text):\n" + rAll.contentsText());
    } else {
        ENTROPY_LOG_ERROR("readAll failed: " + rAll.errorInfo().message);
    }

    // Read range
    auto rRange = handle.readRange(0, 5);
    rRange.wait();
    if (rRange.status() == FileOpStatus::Complete || rRange.status() == FileOpStatus::Partial) {
        auto bytes = rRange.contentsBytes();
        std::string s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ENTROPY_LOG_INFO("readRange(0,5): '" + s + "'");
    }

    // Write range (append-like by offset)
    const char* tail = "\nAppended via writeRange.";
    auto wr = handle.writeRange( rAll.contentsBytes().size(), std::span<const std::byte>(reinterpret_cast<const std::byte*>(tail), strlen(tail)) );
    wr.wait();
    if (wr.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR("writeRange failed");
    }

    // Read line 1 (0-based)
    auto rLine = handle.readLine(1);
    rLine.wait();
    if (rLine.status() == FileOpStatus::Complete) {
        ENTROPY_LOG_INFO("readLine(1): '" + rLine.contentsText() + "'");
    }
    
    // Test writeLine with atomic rename (improved performance)
    auto wLine = handle.writeLine(1, "Modified line two via atomic rename");
    wLine.wait();
    if (wLine.status() == FileOpStatus::Complete) {
        ENTROPY_LOG_INFO("writeLine succeeded with atomic rename");
    } else {
        ENTROPY_LOG_ERROR("writeLine failed: " + wLine.errorInfo().message);
    }
    
    // Test streaming API for large files
    ENTROPY_LOG_INFO("Testing streaming API:");
    auto stream = handle.openReadWriteStream();
    if (stream && stream->good()) {
        // Write some data using stream
        const char* streamData = "\nData written via streaming API";
        stream->seek(0, std::ios::end);
        auto writeResult = stream->write(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(streamData), strlen(streamData)));
        stream->flush();
        ENTROPY_LOG_INFO("Wrote " + std::to_string(writeResult.bytesTransferred) + " bytes via stream");

        // Read back using stream
        stream->seek(0, std::ios::beg);
        std::vector<std::byte> buffer(256);
        auto readResult = stream->read(buffer);
        ENTROPY_LOG_INFO("Read " + std::to_string(readResult.bytesTransferred) + " bytes via stream");
    }
    
    // Test multiple concurrent operations to verify thread safety
    ENTROPY_LOG_INFO("Testing concurrent operations:");
    std::vector<FileOperationHandle> concurrentOps;
    for (int i = 0; i < 5; ++i) {
        auto h = vfs.createFileHandle("concurrent_test_" + std::to_string(i) + ".txt");
        concurrentOps.push_back(h.writeAll("Concurrent write " + std::to_string(i)));
    }

    // Wait for all concurrent writes
    for (auto& op : concurrentOps) {
        op.wait();
        if (op.status() != FileOpStatus::Complete) {
            ENTROPY_LOG_ERROR("Concurrent write failed: " + op.errorInfo().message);
        }
    }
    ENTROPY_LOG_INFO("All concurrent operations completed");
    
    // Demonstrate concurrency: many file operations writing to the SAME file
    ENTROPY_LOG_INFO("1000 file operations writing to the same file (serialized by VFS lock):");
    const std::string sameFile = "vfs_contracts_same_file.txt";
    // Ensure a clean file
    vfs.createFileHandle(sameFile).createEmpty().wait();

    // Create 1000 file operations (not nested work contracts)
    std::vector<FileOperationHandle> writeOps;
    writeOps.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        auto fh = vfs.createFileHandle(sameFile);
        // Each operation writes its own line index. Lines are:
        // "Work contract N wrote!"
        writeOps.push_back(fh.writeLine(static_cast<size_t>(i),
            "Work contract " + std::to_string(i + 1) + " wrote!"));
        ENTROPY_LOG_INFO("Scheduled write " + std::to_string(i + 1));
    }
    // Wait for all write operations to finish
    for (auto& op : writeOps) {
        op.wait();
        if (op.status() != FileOpStatus::Complete) {
            ENTROPY_LOG_ERROR("Write operation failed: " + op.errorInfo().message);
        }
    }
    ENTROPY_LOG_INFO("All 1000 writes completed");

    // Read back the file to show the results
    auto verify = vfs.createFileHandle(sameFile).readAll();
    verify.wait();
    if (verify.status() == FileOpStatus::Complete) {
        ENTROPY_LOG_INFO("Contents of '" + sameFile + "':\n" + verify.contentsText());
    } else {
        ENTROPY_LOG_ERROR("Failed to read back '" + sameFile + "': " + verify.errorInfo().message);
    }

    // Example 4: Batch write operations
    ENTROPY_LOG_INFO("=== Example 4: Batch Write Operations ===");

    auto batchHandle = vfs.createFileHandle("batch_write_test.txt");
    auto batch = vfs.createWriteBatch(batchHandle.metadata().path);

    // Add lines in non-sequential order (demonstrating atomic reordering)
    batch->writeLine(5, "This is line 5");
    batch->writeLine(0, "This is line 0");
    batch->writeLine(3, "This is line 3");
    batch->insertLine(1, "Inserted at line 1");
    batch->appendLine("This line is appended");
    batch->writeLine(2, "This is line 2");

    ENTROPY_LOG_INFO("Batch has " + std::to_string(batch->pendingOperations()) + " operations pending");

    // Commit all operations atomically
    auto batchOp = batch->commit();
    batchOp.wait();

    if (batchOp.status() == FileOpStatus::Complete) {
        ENTROPY_LOG_INFO("Batch write completed successfully, wrote " + std::to_string(batchOp.bytesWritten()) + " bytes");

        // Read back the result
        auto batchRead = batchHandle.readAll();
        batchRead.wait();
        if (batchRead.status() == FileOpStatus::Complete) {
            ENTROPY_LOG_INFO("Contents after batch write:\n" + batchRead.contentsText());
        }
    } else {
        ENTROPY_LOG_ERROR("Batch write failed: " + batchOp.errorInfo().message);
    }

    // Example 5: Efficient bulk update with batch
    ENTROPY_LOG_INFO("=== Example 5: Efficient Bulk Update with Batch ===");

    // Benchmark: Individual writeLine operations
    auto individualHandle = vfs.createFileHandle("individual_write_test.txt");
    individualHandle.createEmpty().wait();

    auto startIndividual = std::chrono::high_resolution_clock::now();
    std::vector<FileOperationHandle> individualOps;
    individualOps.reserve(100);
    for (int i = 0; i < 100; ++i) {
        individualOps.push_back(individualHandle.writeLine(i, "Individual line " + std::to_string(i)));
    }
    for (auto& op : individualOps) {
        op.wait();
    }
    auto endIndividual = std::chrono::high_resolution_clock::now();
    auto individualDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endIndividual - startIndividual).count();

    // Benchmark: Batch operation
    auto bulkHandle = vfs.createFileHandle("bulk_update_test.txt");
    auto bulkBatch = vfs.createWriteBatch(bulkHandle.metadata().path);

    auto startBatch = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; ++i) {
        bulkBatch->writeLine(i, "Batch line " + std::to_string(i));
    }
    auto bulkOp = bulkBatch->commit();
    bulkOp.wait();
    auto endBatch = std::chrono::high_resolution_clock::now();
    auto batchDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endBatch - startBatch).count();

    // Report comparison
    ENTROPY_LOG_INFO("Individual writes (100 lines): " + std::to_string(individualDuration) + "ms");
    ENTROPY_LOG_INFO("Batch write (100 lines): " + std::to_string(batchDuration) + "ms");
    if (individualDuration > 0) {
        float speedup = static_cast<float>(individualDuration) / static_cast<float>(batchDuration > 0 ? batchDuration : 1);
        ENTROPY_LOG_INFO("Batch is " + std::to_string(speedup) + "x faster than individual operations");
    }

    individualHandle.remove().wait();

    // Clean up test files
    for (int i = 0; i < 5; ++i) {
        auto h = vfs.createFileHandle("concurrent_test_" + std::to_string(i) + ".txt");
        h.remove().wait();
    }

    // Example 6: Batch Metadata Queries
    ENTROPY_LOG_INFO("=== Example 6: Batch Metadata Queries ===");

    // Create some test files and keep handles
    std::vector<std::string> testFilePaths;
    std::vector<FileHandle> metadataHandles;
    for (int i = 0; i < 10; ++i) {
        auto h = vfs.createFileHandle("metadata_test_" + std::to_string(i) + ".txt");
        h.writeAll("Test content " + std::to_string(i)).wait();
        testFilePaths.push_back(h.metadata().path);
        metadataHandles.push_back(std::move(h));
    }

    // Query metadata for all files at once
    BatchMetadataOptions batchOpts;
    batchOpts.paths = testFilePaths;

    auto backend = vfs.getDefaultBackend();
    if (backend) {
        auto metaBatch = backend->getMetadataBatch(batchOpts);
        metaBatch.wait();

        if (metaBatch.status() == FileOpStatus::Complete) {
            const auto& metadata = metaBatch.metadataBatch();
            ENTROPY_LOG_INFO("Retrieved metadata for " + std::to_string(metadata.size()) + " files in a single batch");

            for (const auto& meta : metadata) {
                ENTROPY_LOG_INFO("  " + meta.path + ": " +
                    (meta.exists ? "exists" : "missing") + ", size=" + std::to_string(meta.size) + " bytes");
            }
        }
    }

    // Example 7: Copy Operations with Progress
    ENTROPY_LOG_INFO("=== Example 7: Copy Operations with Progress ===");

    // Create a larger source file
    auto sourceHandle = vfs.createFileHandle("large_source.txt");
    auto destHandle = vfs.createFileHandle("large_copy.txt");

    std::string largeContent;
    for (int i = 0; i < 10000; ++i) {
        largeContent += "This is line " + std::to_string(i) + " of the large file.\n";
    }
    sourceHandle.writeAll(largeContent).wait();

    // Copy with progress callback
    CopyOptions copyOpts;
    copyOpts.overwriteExisting = true;
    copyOpts.preserveAttributes = true;
    copyOpts.progressCallback = [](size_t copied, size_t total) {
        int percent = static_cast<int>((copied * 100) / total);
        if (percent % 10 == 0 && copied > 0) {
            ENTROPY_LOG_INFO("  Copy progress: " + std::to_string(percent) + "% (" +
                std::to_string(copied) + "/" + std::to_string(total) + " bytes)");
        }
        return true;
    };

    if (backend) {
        auto copyOp = backend->copyFile(sourceHandle.metadata().path, destHandle.metadata().path, copyOpts);
        copyOp.wait();

        if (copyOp.status() == FileOpStatus::Complete) {
            ENTROPY_LOG_INFO("Copy completed: " + std::to_string(copyOp.bytesWritten()) + " bytes copied");
        } else {
            ENTROPY_LOG_ERROR("Copy failed: " + copyOp.errorInfo().message);
        }
    }

    // Example 8: Move Operations
    ENTROPY_LOG_INFO("=== Example 8: Move Operations ===");

    auto moveSourceHandle = vfs.createFileHandle("file_to_move.txt");
    auto moveDestHandle = vfs.createFileHandle("moved_file.txt");

    moveSourceHandle.writeAll("This file will be moved").wait();

    if (backend) {
        auto moveOp = backend->moveFile(moveSourceHandle.metadata().path, moveDestHandle.metadata().path, false);
        moveOp.wait();

        if (moveOp.status() == FileOpStatus::Complete) {
            ENTROPY_LOG_INFO("Move completed successfully");
            ENTROPY_LOG_INFO("  Source exists: " + std::to_string(std::filesystem::exists(moveSourceHandle.metadata().path)));
            ENTROPY_LOG_INFO("  Dest exists: " + std::to_string(std::filesystem::exists(moveDestHandle.metadata().path)));
        } else {
            ENTROPY_LOG_ERROR("Move failed: " + moveOp.errorInfo().message);
        }

        // Test move with overwrite
        auto moveSource2 = vfs.createFileHandle("file_to_move2.txt");
        moveSource2.writeAll("Second file to move").wait();

        auto moveOpOverwrite = backend->moveFile(moveSource2.metadata().path, moveDestHandle.metadata().path, true);
        moveOpOverwrite.wait();

        if (moveOpOverwrite.status() == FileOpStatus::Complete) {
            ENTROPY_LOG_INFO("Move with overwrite completed successfully");
        } else {
            ENTROPY_LOG_ERROR("Move with overwrite failed: " + moveOpOverwrite.errorInfo().message);
        }
    }

    // Example 9: Optimized Buffering
    ENTROPY_LOG_INFO("=== Example 9: Optimized 64KB Buffering ===");

    auto bufferedStream = handle.openBufferedStream();
    if (bufferedStream && bufferedStream->good()) {
        ENTROPY_LOG_INFO("Opened buffered stream with 64KB buffer for optimal performance");
    }

    // Example 10: File Watching
    ENTROPY_LOG_INFO("=== Example 10: File System Watching ===");

    // Create a directory to watch
    std::filesystem::create_directory("watch_test_dir");

    // Set up watch options with filtering
    WatchOptions watchOpts;
    watchOpts.recursive = true;
    watchOpts.includePatterns = {"*.txt", "*.log"};  // Only watch text and log files
    watchOpts.excludePatterns = {"*.tmp", "*.temp"}; // Ignore temp files

    // Create a watch with a callback
    std::atomic<int> eventCount{0};
    auto watch = vfs.watchDirectory("watch_test_dir", [&eventCount](const FileWatchInfo& info) {
        eventCount++;
        std::string eventType;
        switch (info.event) {
            case FileWatchEvent::Created:  eventType = "Created"; break;
            case FileWatchEvent::Modified: eventType = "Modified"; break;
            case FileWatchEvent::Deleted:  eventType = "Deleted"; break;
            case FileWatchEvent::Renamed:  eventType = "Renamed"; break;
        }

        ENTROPY_LOG_INFO("File event: " + eventType + " - " + info.path);
        if (info.oldPath.has_value()) {
            ENTROPY_LOG_INFO("  Old path: " + info.oldPath.value());
        }
    }, watchOpts);

    if (watch && watch->isWatching()) {
        ENTROPY_LOG_INFO("Started watching directory: watch_test_dir");

        // Trigger some file events
        auto testFile1 = vfs.createFileHandle("watch_test_dir/test1.txt");
        testFile1.writeAll("This file should trigger a Created event").wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto testFile2 = vfs.createFileHandle("watch_test_dir/test2.log");
        testFile2.writeAll("Log file created").wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // This file should be ignored due to exclude pattern
        auto tempFile = vfs.createFileHandle("watch_test_dir/ignored.tmp");
        tempFile.writeAll("This should be ignored").wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Modify a file
        testFile1.writeAll("Modified content").wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Delete a file
        testFile2.remove().wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Allow time for events to be processed
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        ENTROPY_LOG_INFO("File watch received " + std::to_string(eventCount.load()) + " events");

        // Stop watching
        watch->stop();
        ENTROPY_LOG_INFO("Stopped watching directory");

        // Clean up watch (decrements refcount)
        vfs.unwatchDirectory(watch);
    } else {
        ENTROPY_LOG_ERROR("Failed to create file watch");
    }

    // Clean up watch test directory
    std::filesystem::remove_all("watch_test_dir");

    // Clean up test files
    ENTROPY_LOG_INFO("Cleaning up test files...");
    for (auto& h : metadataHandles) {
        h.remove().wait();
    }

    // Clean up service
    service.stop();
    return 0;
}
