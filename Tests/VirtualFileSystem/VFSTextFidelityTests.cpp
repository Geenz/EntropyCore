#include <catch2/catch_test_macros.hpp>
#include "EntropyCore.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/WriteBatch.h"
#include <vector>
#include <cstring>
#include <fstream>
#include <filesystem>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

static std::filesystem::path makeTempPath(const std::string& base) {
    auto dir = std::filesystem::temp_directory_path();
    std::random_device rd;
    for (int i = 0; i < 8; ++i) {
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        auto name = base + "-" + std::to_string(now) + "-" + std::to_string(rd());
        auto p = dir / name;
        if (!std::filesystem::exists(p)) return p;
    }
    return dir / (base + "-" + std::to_string(rd()));
}

static std::string readAllBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::in | std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

static void startService(WorkService& svc, WorkContractGroup& group) {
    svc.start();
    svc.addWorkContractGroup(&group);
}

TEST_CASE("writeLine preserves dominant EOL and final-newline policy (CRLF)", "[vfs][eol][writeLine]") {
    WorkService svc({});
    WorkContractGroup group(256, "VfsTestGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    auto temp = makeTempPath("vfs_eol_crlf");
    // Create file with CRLF and final newline
    {
        std::ofstream out(temp, std::ios::out | std::ios::binary | std::ios::trunc);
        out << "A\r\nB\r\n"; // final newline present
    }

    auto fh = vfs.createFileHandle(temp.string());
    auto wl = fh.writeLine(1, "B_replaced");
    wl.wait();
    REQUIRE(wl.status() == FileOpStatus::Complete);

    auto bytes = readAllBytes(temp);
    // Expect CRLF and final newline preserved
    REQUIRE(bytes == std::string("A\r\nB_replaced\r\n"));

    svc.stop();
    std::error_code ec; (void)std::filesystem::remove(temp, ec);
}

TEST_CASE("writeLine preserves dominant EOL and trailing-newline absence (LF)", "[vfs][eol][writeLine]") {
    WorkService svc({});
    WorkContractGroup group(256, "VfsTestGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    auto temp = makeTempPath("vfs_eol_lf");
    // Create file with LF and no trailing newline
    {
        std::ofstream out(temp, std::ios::out | std::ios::binary | std::ios::trunc);
        out << "Line1\nLine2"; // no final newline
    }

    auto fh = vfs.createFileHandle(temp.string());
    auto wl = fh.writeLine(0, "First");
    wl.wait();
    REQUIRE(wl.status() == FileOpStatus::Complete);

    auto bytes = readAllBytes(temp);
    REQUIRE(bytes == std::string("First\nLine2")); // LF and no final newline

    svc.stop();
    std::error_code ec; (void)std::filesystem::remove(temp, ec);
}

TEST_CASE("WriteBatch commit honors ensureFinalNewline override", "[vfs][eol][batch]") {
    WorkService svc({});
    WorkContractGroup group(256, "VfsTestGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    // Start with LF and no trailing newline
    auto temp = makeTempPath("vfs_batch_finalnl");
    {
        std::ofstream out(temp, std::ios::out | std::ios::binary | std::ios::trunc);
        out << "One\nTwo";
    }

    // ensureFinalNewline = true
    {
        auto batch = vfs.createWriteBatch(temp.string());
        batch->writeLine(1, "Deux");
        WriteOptions opts{}; opts.ensureFinalNewline = true;
        auto h = batch->commit(opts); h.wait();
        REQUIRE(h.status() == FileOpStatus::Complete);
        auto bytes = readAllBytes(temp);
        // Expect LF with final newline present
        REQUIRE(bytes == std::string("One\nDeux\n"));
    }

    // ensureFinalNewline = false
    {
        auto batch = vfs.createWriteBatch(temp.string());
        batch->writeLine(0, "Un");
        WriteOptions opts{}; opts.ensureFinalNewline = false;
        auto h = batch->commit(opts); h.wait();
        REQUIRE(h.status() == FileOpStatus::Complete);
        auto bytes = readAllBytes(temp);
        // Expect LF with no final newline
        REQUIRE(bytes == std::string("Un\nDeux"));
    }

    svc.stop();
    std::error_code ec; (void)std::filesystem::remove(temp, ec);
}

TEST_CASE("WriteBatch createParentDirs per-commit override", "[vfs][batch][dirs]") {
    WorkService svc({});
    WorkContractGroup group(256, "VfsTestGroup");
    startService(svc, group);
    // Default: do not auto-create parents
    VirtualFileSystem::Config cfg; cfg.defaultCreateParentDirs = false;
    VirtualFileSystem vfs(&group, cfg);

    auto root = std::filesystem::temp_directory_path() / makeTempPath("vfs_parent").filename();
    auto nested = root / "a" / "b" / "c.txt";
    // Make sure parent dirs do not exist
    std::error_code ec; std::filesystem::remove_all(root, ec);

    // No override -> should fail
    {
        auto batch = vfs.createWriteBatch(nested.string());
        batch->writeLine(0, "Hello");
        auto h = batch->commit(); h.wait();
        REQUIRE(h.status() == FileOpStatus::Failed);
    }

    // Override true -> should succeed and create parents
    {
        auto batch = vfs.createWriteBatch(nested.string());
        batch->writeLine(0, "Hello");
        WriteOptions opts{}; opts.createParentDirs = true;
        auto h = batch->commit(opts); h.wait();
        REQUIRE(h.status() == FileOpStatus::Complete);
        REQUIRE(std::filesystem::exists(nested));
    }

    svc.stop();
    std::filesystem::remove_all(root, ec);
}
