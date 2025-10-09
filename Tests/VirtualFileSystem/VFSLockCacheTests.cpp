#include <catch2/catch_test_macros.hpp>
#include "EntropyCore.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/FileHandle.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include <filesystem>
#include <thread>
#include <chrono>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

static std::string tempPathLC(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

TEST_CASE("VFS lock cache: eviction stress then serialize on same key", "[vfs][locks][eviction]") {
    using namespace std::chrono_literals;

    WorkService svc({});
    WorkContractGroup group(128, "VFS_LockCache");
    svc.start();
    svc.addWorkContractGroup(&group);

    VirtualFileSystem::Config cfg;
    cfg.serializeWritesPerPath = true;
    cfg.maxWriteLocksCached = 4;               // small cache to force eviction
    cfg.writeLockTimeout = std::chrono::minutes(0); // immediate eligible for eviction
    cfg.advisoryFallback = VirtualFileSystem::Config::AdvisoryFallbackPolicy::FallbackWithTimeout;
    cfg.advisoryAcquireTimeout = 50ms;         // bounded advisory wait

    VirtualFileSystem vfs(&group, cfg);

    // Stress the lock cache with many distinct keys
    for (int i = 0; i < 20; ++i) {
        auto fh = vfs.createFileHandle(tempPathLC("evict_key_" + std::to_string(i) + ".txt"));
        WriteOptions wo; wo.truncate = true; wo.createIfMissing = true;
        auto w = fh.writeAll("x", wo); w.wait();
        REQUIRE((w.status() == FileOpStatus::Complete));
    }

    // Now test that serialization still works for a fresh key after prior evictions
    // Use the debug seam to hold the serialized section to trigger a timeout for a competing writer
    _putenv_s("ENTROPY_TEST_HOLD_WRITE_LOCK_MS", "200");

    auto contested = vfs.createFileHandle(tempPathLC("EvictAfter_HoldLock.txt"));

    WriteOptions wo; wo.truncate = true; wo.createIfMissing = true;
    auto op1 = contested.writeAll("lock", wo); // first writer, will hold serialized section

    // Give a moment for op1 to start and acquire the advisory lock
    std::this_thread::sleep_for(10ms);

    // Second writer on same key should time out on advisory try_lock_for
    auto op2 = contested.writeAll("second", wo);
    op2.wait();
    REQUIRE(op2.status() == FileOpStatus::Failed);
    const auto& e2 = op2.errorInfo();
    REQUIRE(e2.code == FileError::Timeout);
    REQUIRE(e2.message.find("Advisory lock acquisition timed out") != std::string::npos);
    REQUIRE(e2.message.find("(key=") != std::string::npos);

    _putenv_s("ENTROPY_TEST_HOLD_WRITE_LOCK_MS", "0");

    svc.stop();
}

TEST_CASE("VFS lock cache: post-eviction same-key writers still serialize without failure", "[vfs][locks][eviction]") {
    using namespace std::chrono_literals;

    WorkService svc({});
    WorkContractGroup group(128, "VFS_LockCache2");
    svc.start();
    svc.addWorkContractGroup(&group);

    VirtualFileSystem::Config cfg;
    cfg.serializeWritesPerPath = true;
    cfg.maxWriteLocksCached = 3;               // even smaller cache
    cfg.writeLockTimeout = std::chrono::minutes(0);
    cfg.advisoryFallback = VirtualFileSystem::Config::AdvisoryFallbackPolicy::FallbackWithTimeout;
    cfg.advisoryAcquireTimeout = 50ms;

    VirtualFileSystem vfs(&group, cfg);

    // Evict by touching many keys
    for (int i = 0; i < 10; ++i) {
        auto fh = vfs.createFileHandle(tempPathLC("evict2_key_" + std::to_string(i) + ".txt"));
        WriteOptions wo; wo.truncate = true; wo.createIfMissing = true;
        auto w = fh.writeAll("y", wo); w.wait();
        REQUIRE((w.status() == FileOpStatus::Complete));
    }

    // Same key, back-to-back writes should not fail and should be serialized internally
    auto test = vfs.createFileHandle(tempPathLC("post_evict_sequence.txt"));
    WriteOptions wo; wo.truncate = true; wo.createIfMissing = true;

    auto w1 = test.writeAll("A", wo); w1.wait();
    REQUIRE(w1.status() == FileOpStatus::Complete);

    auto w2 = test.writeAll("B", wo); w2.wait();
    REQUIRE(w2.status() == FileOpStatus::Complete);

    // Confirm contents is the latter value (best-effort)
    auto r = test.readAll(); r.wait();
    REQUIRE((r.status() == FileOpStatus::Complete));
    auto bytes = r.contentsBytes();
    REQUIRE(!bytes.empty());
    REQUIRE(static_cast<char>(bytes.back()) == 'B');

    // Cleanup is implicit; tests avoid std::filesystem destructive ops per guidelines
    test.remove().wait();

    svc.stop();
}
