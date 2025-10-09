#include <catch2/catch_test_macros.hpp>
#include "EntropyCore.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/IFileSystemBackend.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <thread>
#include <chrono>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

static std::filesystem::path makeTempPathEM(const std::string& base) {
    auto dir = std::filesystem::temp_directory_path();
    std::random_device rd;
    auto name = base + "-" + std::to_string(rd());
    return dir / name;
}

TEST_CASE("Error taxonomy: read missing file -> FileNotFound", "[vfs][errors]") {
    WorkService svc({});
    WorkContractGroup group(64, "ErrMap");
    svc.start();
    svc.addWorkContractGroup(&group);
    VirtualFileSystem vfs(&group);

    auto missing = makeTempPathEM("missing_file");
    std::error_code ec; std::filesystem::remove(missing, ec);

    // Ensure default backend is initialized
    if (!vfs.getDefaultBackend()) {
        (void)vfs.createFileHandle(missing.string());
    }
    auto backend = vfs.getDefaultBackend();
    REQUIRE(backend);
    ReadOptions ro{};
    auto op = backend->readFile(missing.string(), ro); op.wait();
    REQUIRE(op.status() == FileOpStatus::Failed);
    const auto& err = op.errorInfo();
    REQUIRE(err.code == FileError::FileNotFound);

    svc.stop();
}

TEST_CASE("Error taxonomy: write to read-only file -> AccessDenied", "[vfs][errors][perms]") {
    WorkService svc({});
    WorkContractGroup group(64, "ErrMap");
    svc.start();
    svc.addWorkContractGroup(&group);
    VirtualFileSystem vfs(&group);

    auto p = makeTempPathEM("readonly_file");
    // Create file
    {
        std::ofstream out(p, std::ios::binary); out << "x"; out.close();
    }
    // Make it read-only (best-effort across platforms)
    std::error_code ec;
    auto perms = std::filesystem::status(p, ec).permissions();
    if (!ec) {
        std::filesystem::permissions(p,
            std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write,
            std::filesystem::perm_options::remove, ec);
    }

    auto backend = vfs.getDefaultBackend(); REQUIRE(backend);
    std::string text = "cannot write";
    WriteOptions wo; wo.truncate = true;
    auto op = backend->writeFile(p.string(), std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()), wo);
    op.wait();
    REQUIRE(op.status() == FileOpStatus::Failed);
    const auto& err = op.errorInfo();
    REQUIRE(err.code == FileError::AccessDenied);
    // systemError may or may not be present depending on platform; if present, it's informative

    // Cleanup and restore permissions for removal
    std::filesystem::permissions(p,
        std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add, ec);
    std::filesystem::remove(p, ec);

    svc.stop();
}

TEST_CASE("Error taxonomy: invalid path -> InvalidPath (Windows)", "[vfs][errors][invalid]") {
#if defined(_WIN32)
    WorkService svc({});
    WorkContractGroup group(64, "ErrMap");
    svc.start();
    svc.addWorkContractGroup(&group);
    VirtualFileSystem vfs(&group);

    auto dir = std::filesystem::temp_directory_path();
    // Use an illegal character '?' in the filename on Windows
    auto bad = (dir / "bad?name.txt").string();

    auto backend = vfs.getDefaultBackend(); REQUIRE(backend);
    std::string text = "data";
    WriteOptions wo; wo.truncate = true;
    auto op = backend->writeFile(bad, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()), wo);
    op.wait();
    REQUIRE(op.status() == FileOpStatus::Failed);
    const auto& err = op.errorInfo();
    REQUIRE(err.code == FileError::InvalidPath);

    svc.stop();
#endif
}

TEST_CASE("Error taxonomy: Disk full mapping via test seam", "[vfs][errors][diskfull]") {
    using namespace std::chrono_literals;
    WorkService svc({});
    WorkContractGroup group(64, "ErrMap");
    svc.start();
    svc.addWorkContractGroup(&group);
    VirtualFileSystem vfs(&group);

    _putenv_s("ENTROPY_TEST_SIMULATE_DISK_FULL", "1");

    auto p = makeTempPathEM("SimulateDiskFull_file");
    auto backend = vfs.getDefaultBackend(); REQUIRE(backend);
    std::string text = "data";
    WriteOptions wo; wo.truncate = true;
    auto op = backend->writeFile(p.string(), std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()), wo);
    op.wait();
    REQUIRE(op.status() == FileOpStatus::Failed);
    const auto& err = op.errorInfo();
    REQUIRE(err.code == FileError::DiskFull);
    REQUIRE(err.systemError.has_value());

    _putenv_s("ENTROPY_TEST_SIMULATE_DISK_FULL", "0");
    std::error_code ec; std::filesystem::remove(p, ec);
    svc.stop();
}

TEST_CASE("Advisory fallback timeout message includes duration and key", "[vfs][errors][timeout]") {
    using namespace std::chrono_literals;
    WorkService svc({});
    WorkContractGroup group(64, "ErrMap");
    svc.start();
    svc.addWorkContractGroup(&group);

    VirtualFileSystem::Config cfg;
    cfg.advisoryFallback = VirtualFileSystem::Config::AdvisoryFallbackPolicy::FallbackWithTimeout;
    cfg.advisoryAcquireTimeout = 50ms;
    VirtualFileSystem vfs(&group, cfg);

    auto p = makeTempPathEM("HoldLock_timeout");
    auto backend = vfs.getDefaultBackend(); REQUIRE(backend);

    _putenv_s("ENTROPY_TEST_HOLD_WRITE_LOCK_MS", "200");

    // Schedule a first write that holds the serialized section briefly
    std::string text = "lock";
    WriteOptions wo; wo.truncate = true;
    auto op1 = backend->writeFile(p.string() + "_HoldLock", std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()), wo);

    // Give it a moment to start and acquire the lock
    std::this_thread::sleep_for(10ms);

    // Second write should time out on advisory try_lock_for
    auto op2 = backend->writeFile((p.string() + "_HoldLock"), std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()), wo);
    op2.wait();
    REQUIRE(op2.status() == FileOpStatus::Failed);
    const auto& e2 = op2.errorInfo();
    REQUIRE(e2.code == FileError::Timeout);
    REQUIRE(e2.message.find("Advisory lock acquisition timed out after 50 ms") != std::string::npos);
    REQUIRE(e2.message.find("(key=") != std::string::npos);

    _putenv_s("ENTROPY_TEST_HOLD_WRITE_LOCK_MS", "0");
    std::error_code ec; std::filesystem::remove(p.string() + "_HoldLock", ec);
    svc.stop();
}

TEST_CASE("Scope acquisition Error maps to IOError with code/message", "[vfs][errors][scope]") {
    WorkService svc({});
    WorkContractGroup group(64, "ErrMap");
    svc.start();
    svc.addWorkContractGroup(&group);

    VirtualFileSystem::Config cfg;
    cfg.advisoryFallback = VirtualFileSystem::Config::AdvisoryFallbackPolicy::None;
    VirtualFileSystem vfs(&group, cfg);
    auto backend = vfs.getDefaultBackend(); REQUIRE(backend);

    _putenv_s("ENTROPY_TEST_FORCE_SCOPE_ERROR", "1");

    auto p = makeTempPathEM("SimulateScopeError_file");
    std::string text = "data";
    WriteOptions wo; wo.truncate = true;
    auto op = backend->writeFile(p.string(), std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()), wo);
    op.wait();
    REQUIRE(op.status() == FileOpStatus::Failed);
    const auto& err = op.errorInfo();
    REQUIRE(err.code == FileError::IOError);
    REQUIRE(err.systemError.has_value());
    REQUIRE(err.message.find("Simulated backend scope acquisition error") != std::string::npos);

    _putenv_s("ENTROPY_TEST_FORCE_SCOPE_ERROR", "0");
    std::error_code ec; std::filesystem::remove(p, ec);
    svc.stop();
}
