#include <catch2/catch_test_macros.hpp>
#include "EntropyCore.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/IFileSystemBackend.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include <filesystem>
#include <fstream>

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
