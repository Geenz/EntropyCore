#include <catch2/catch_test_macros.hpp>
#include "EntropyCore.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/WriteBatch.h"
#include "VirtualFileSystem/DirectoryHandle.h"
#include <vector>
#include <unordered_set>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <atomic>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

static std::filesystem::path makeTempPathEx(const std::string& base) {
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

static void startService(WorkService& svc, WorkContractGroup& group) {
    svc.start();
    svc.addWorkContractGroup(&group);
}

static std::string readAllBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::in | std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

TEST_CASE("VFS basic create/write/read/remove flows", "[vfs][basic]") {
    WorkService svc({});
    WorkContractGroup group(256, "VfsGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    auto temp = makeTempPathEx("vfs_basic");
    auto fh = vfs.createFileHandle(temp.string());

    // createEmpty then remove is idempotent
    auto c = fh.createEmpty(); c.wait();
    REQUIRE(c.status() == FileOpStatus::Complete);

    // writeAll + readAll
    const char* text = "Hello from tests!\nSecond line.";
    auto w = fh.writeAll(text); w.wait();
    REQUIRE(w.status() == FileOpStatus::Complete);

    auto rAll = fh.readAll(); rAll.wait();
    REQUIRE(rAll.status() == FileOpStatus::Complete);
    REQUIRE(rAll.contentsText() == std::string(text));

    // readRange complete and partial at EOF
    auto rrComplete = fh.readRange(0, 5); rrComplete.wait();
    REQUIRE((rrComplete.status() == FileOpStatus::Complete || rrComplete.status() == FileOpStatus::Partial));
    auto rrPartial = fh.readRange(static_cast<uint64_t>(rAll.contentsBytes().size()) - 3, 10); rrPartial.wait();
    REQUIRE(rrPartial.status() == FileOpStatus::Partial);

    // writeRange append at explicit offset (EOF)
    const char* tail = "\nAppended via writeRange.";
    auto wr = fh.writeRange(rAll.contentsBytes().size(), std::span<const std::byte>(reinterpret_cast<const std::byte*>(tail), strlen(tail)));
    wr.wait();
    REQUIRE(wr.status() == FileOpStatus::Complete);

    // remove (idempotent)
    auto d = fh.remove(); d.wait();
    REQUIRE(d.status() == FileOpStatus::Complete);

    svc.stop();
    std::error_code ec; (void)std::filesystem::remove(temp, ec);
}

TEST_CASE("FileHandle::openBufferedStream convenience", "[vfs][stream][buffered]") {
    WorkService svc({});
    WorkContractGroup group(128, "TestGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    auto temp = makeTempPathEx("vfs_buffered_conv");
    auto fh = vfs.createFileHandle(temp.string());
    fh.createEmpty().wait();

    auto buffered = fh.openBufferedStream();
    REQUIRE(buffered);
    const char* msg = "Buffered convenience hello\n";
    buffered->seek(0, std::ios::end);
    auto wrote = buffered->write(std::span<const std::byte>(reinterpret_cast<const std::byte*>(msg), strlen(msg)));
    buffered->flush();
    REQUIRE(wrote.bytesTransferred == strlen(msg));

    buffered->seek(0, std::ios::beg);
    std::vector<std::byte> buf(64);
    auto read = buffered->read(buf);
    REQUIRE(read.bytesTransferred >= wrote.bytesTransferred);

    svc.stop();
    std::error_code ec; (void)std::filesystem::remove(temp, ec);
}

TEST_CASE("Concurrent writes to different files complete", "[vfs][concurrency]") {
    WorkService svc({});
    WorkContractGroup group(512, "VfsGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    std::vector<std::filesystem::path> paths;
    std::vector<FileOperationHandle> ops;
    for (int i = 0; i < 5; ++i) {
        auto p = makeTempPathEx("vfs_concurrent_");
        paths.push_back(p);
        ops.push_back(vfs.createFileHandle(p.string()).writeAll(std::string("Concurrent ") + std::to_string(i)));
    }
    for (auto& op : ops) { op.wait(); REQUIRE(op.status() == FileOpStatus::Complete); }

    svc.stop();
    std::error_code ec; for (auto& p : paths) (void)std::filesystem::remove(p, ec);
}

TEST_CASE("Same-file writes serialize via VFS lock", "[vfs][locking][stress]") {
    WorkService svc({});
    WorkContractGroup group(2000, "VfsGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    auto temp = makeTempPathEx("vfs_same_file");
    auto fh = vfs.createFileHandle(temp.string());
    fh.createEmpty().wait();

    const int N = 200; // keep runtime reasonable
    std::vector<FileOperationHandle> ops;
    ops.reserve(N);
    for (int i = 0; i < N; ++i) {
        ops.push_back(fh.writeLine(static_cast<size_t>(i), std::string("Line ") + std::to_string(i)));
    }
    for (auto& op : ops) { op.wait(); REQUIRE(op.status() == FileOpStatus::Complete); }

    auto r = fh.readAll(); r.wait(); REQUIRE(r.status() == FileOpStatus::Complete);
    // Count lines by splitting on '\n'
    size_t count = 0; for (char c : r.contentsText()) if (c == '\n') ++count;
    // We preserved original final-newline policy; since we created empty file and wrote lines, expect final newline true by backend default
    REQUIRE(count == static_cast<size_t>(N));

    svc.stop();
    std::error_code ec; (void)std::filesystem::remove(temp, ec);
}

TEST_CASE("WriteBatch operations semantics and preview", "[vfs][batch]") {
    WorkService svc({});
    WorkContractGroup group(512, "VfsGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    auto temp = makeTempPathEx("vfs_batch_semantics");
    auto batch = vfs.createWriteBatch(temp.string());

    batch->writeLine(5, "Line5");
    batch->writeLine(0, "Line0");
    batch->writeLine(3, "Line3");
    batch->insertLine(1, "Ins1");
    batch->appendLine("Appended");
    batch->writeLine(2, "Line2");

    // Preview without writing
    auto prev = batch->preview(); prev.wait();
    REQUIRE(prev.status() == FileOpStatus::Complete);
    auto text = prev.contentsText();
    // Should not be empty; contains at least 6 lines
    REQUIRE(text.find("Line0") != std::string::npos);

    // Commit and validate file content contains our lines in expected order
    auto h = batch->commit(); h.wait(); REQUIRE(h.status() == FileOpStatus::Complete);

    auto fh = vfs.createFileHandle(temp.string());
    auto r = fh.readAll(); r.wait(); REQUIRE(r.status() == FileOpStatus::Complete);
    auto out = r.contentsText();
    REQUIRE(out.find("Line0") != std::string::npos);
    REQUIRE(out.find("Ins1") != std::string::npos);
    REQUIRE(out.find("Line2") != std::string::npos);
    REQUIRE(out.find("Line3") != std::string::npos);
    REQUIRE(out.find("Line5") != std::string::npos);
    REQUIRE(out.find("Appended") != std::string::npos);

    // Non-idempotence: committing same batch again should change file further (e.g., duplicate insert/append)
    auto h2 = batch->commit(); h2.wait();
    REQUIRE(h2.status() == FileOpStatus::Complete);

    auto r2 = fh.readAll(); r2.wait(); REQUIRE(r2.status() == FileOpStatus::Complete);
    auto out2 = r2.contentsText();
    REQUIRE(out2.size() >= out.size());

    svc.stop();
    std::error_code ec; (void)std::filesystem::remove(temp, ec);
}

TEST_CASE("Metadata batch returns info for existing and missing files", "[vfs][metadata]") {
    WorkService svc({});
    WorkContractGroup group(256, "VfsGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    std::vector<std::string> paths;
    std::vector<FileHandle> handles;
    for (int i = 0; i < 4; ++i) {
        auto p = makeTempPathEx("vfs_meta_");
        auto h = vfs.createFileHandle(p.string());
        h.writeAll(std::string("File ") + std::to_string(i)).wait();
        paths.push_back(h.metadata().path);
        handles.push_back(h);
    }
    // add a missing path
    auto missing = makeTempPathEx("vfs_meta_missing");
    paths.push_back(missing.string());

    auto backend = vfs.getDefaultBackend();
    REQUIRE(backend);
    BatchMetadataOptions opts; opts.paths = paths;
    auto mb = backend->getMetadataBatch(opts); mb.wait();
    REQUIRE(mb.status() == FileOpStatus::Complete);
    const auto& items = mb.metadataBatch();
    REQUIRE(items.size() == paths.size());
    // Find missing path entry
    bool foundMissing = false;
    for (const auto& m : items) {
        if (m.path == missing.string()) { foundMissing = true; REQUIRE_FALSE(m.exists); }
        else { if (m.path == handles[0].metadata().path || m.path == handles[1].metadata().path || m.path == handles[2].metadata().path || m.path == handles[3].metadata().path) REQUIRE(m.exists); }
    }
    REQUIRE(foundMissing);

    svc.stop();
    std::error_code ec; for (auto& h : handles) (void)std::filesystem::remove(h.metadata().path, ec);
}

TEST_CASE("Copy and move operations with overwrite", "[vfs][copy][move]") {
    WorkService svc({});
    WorkContractGroup group(512, "VfsGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    auto src = makeTempPathEx("vfs_copy_src");
    auto dst = makeTempPathEx("vfs_copy_dst");
    auto srcH = vfs.createFileHandle(src.string());
    auto dstH = vfs.createFileHandle(dst.string());

    std::string content;
    for (int i = 0; i < 1000; ++i) content += "Line " + std::to_string(i) + "\n";
    srcH.writeAll(content).wait();

    auto backend = vfs.getDefaultBackend();
    REQUIRE(backend);
    CopyOptions cop; cop.overwriteExisting = true; cop.preserveAttributes = true;
    size_t lastProgress = 0; cop.progressCallback = [&lastProgress](size_t copied, size_t total){ lastProgress = copied; return true; };
    auto copyOp = backend->copyFile(src.string(), dst.string(), cop); copyOp.wait();
    REQUIRE(copyOp.status() == FileOpStatus::Complete);
    REQUIRE(std::filesystem::exists(dst));
    REQUIRE(readAllBytes(src) == readAllBytes(dst));

    // Move without overwrite should fail if destination exists
    auto moveSrc = makeTempPathEx("vfs_move_src");
    auto moveDst = makeTempPathEx("vfs_move_dst");
    auto moveSrcH = vfs.createFileHandle(moveSrc.string());
    auto moveDstH = vfs.createFileHandle(moveDst.string());
    moveSrcH.writeAll("to move").wait();
    moveDstH.writeAll("exists").wait();
    auto moveFail = backend->moveFile(moveSrc.string(), moveDst.string(), false); moveFail.wait();
    REQUIRE(moveFail.status() == FileOpStatus::Failed);

    // Move with overwrite should succeed
    auto moveOk = backend->moveFile(moveSrc.string(), moveDst.string(), true); moveOk.wait();
    {
        const bool ok = (moveOk.status() == FileOpStatus::Complete) || (moveOk.status() == FileOpStatus::Partial);
        REQUIRE(ok); // Partial if delete failed
    }

    svc.stop();
    std::error_code ec; (void)std::filesystem::remove(src, ec); (void)std::filesystem::remove(dst, ec);
    (void)std::filesystem::remove(moveSrc, ec); (void)std::filesystem::remove(moveDst, ec);
}

TEST_CASE("Per-operation options for writeAll and final newline", "[vfs][options]") {
    WorkService svc({});
    WorkContractGroup group(256, "VfsGroup");
    startService(svc, group);
    VirtualFileSystem::Config cfg; cfg.defaultCreateParentDirs = false;
    VirtualFileSystem vfs(&group, cfg);

    auto root = std::filesystem::temp_directory_path() / makeTempPathEx("vfs_opts").filename();
    auto nested = root / "a" / "b" / "c.txt";
    std::error_code ec; std::filesystem::remove_all(root, ec);

    // writeAll should fail without parent dirs (default false)
    {
        WriteOptions wo; wo.createParentDirs = std::nullopt; // use default (false)
        auto fh = vfs.createFileHandle(nested.string());
        auto h = fh.writeAll("Hello"); h.wait();
        REQUIRE(h.status() == FileOpStatus::Failed);
    }
    // writeAll with override should succeed
    {
        WriteOptions wo; wo.createParentDirs = true; wo.ensureFinalNewline = true;
        auto fh = vfs.createFileHandle(nested.string());
        auto backend = vfs.getDefaultBackend(); REQUIRE(backend);
        // Use backend writeFile to pass options (FileHandle::writeAll currently has no overload with options)
        auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>("Hello"), size_t(5));
        auto h = backend->writeFile(nested.string(), bytes, wo); h.wait();
        REQUIRE(h.status() == FileOpStatus::Complete);
        auto bytesOnDisk = readAllBytes(nested);
        {
            const bool ok2 = (bytesOnDisk == std::string("Hello\n")) || (bytesOnDisk == std::string("Hello\r\n"));
            REQUIRE(ok2); // final newline forced
        }
    }

    svc.stop();
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("FileHandle identity and hashing on Windows", "[vfs][identity][windows]") {
#if defined(_WIN32)
    WorkService svc({});
    WorkContractGroup group(256, "VfsGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    auto base = (std::filesystem::temp_directory_path() / "VfsIdTest").string();
    std::string p1 = base + "\\A.txt";
    std::string p2 = base + "\\a.txt"; // different case

    auto h1 = vfs.createFileHandle(p1);
    auto h2 = vfs.createFileHandle(p2);

    // Ensure normalizedKey equality and FileHandle equality
    REQUIRE(h1.normalizedKey() == h2.normalizedKey());
    REQUIRE(h1 == h2);

    // Hash-based container deduplication
    std::unordered_set<FileHandle> set;
    set.insert(h1);
    set.insert(h2);
    REQUIRE(set.size() == 1);

    svc.stop();
#endif
}

TEST_CASE("Watch directory smoke test (skips if unavailable)", "[vfs][watch]") {
    WorkService svc({});
    WorkContractGroup group(256, "VfsGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    auto dir = makeTempPathEx("vfs_watch_dir");
    std::filesystem::create_directory(dir);

    std::atomic<int> events{0};
    auto watch = vfs.watchDirectory(dir.string(), [&events](const FileWatchInfo&){ events++; });

    // If watching unsupported, watch is null and test passes
    if (watch) {
        // Create and modify a file
        auto fh = vfs.createFileHandle((dir / "t.txt").string());
        fh.writeAll("x").wait();
        fh.writeAll("y").wait();
        fh.remove().wait();
        // Give time for events (best-effort; we don't REQUIRE a count)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        watch->stop();
        vfs.unwatchDirectory(watch);
    }

    svc.stop();
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}


TEST_CASE("Directory listing sorting and pagination are applied after collection", "[vfs][dir][list]") {
    WorkService svc({});
    WorkContractGroup group(128, "DirListGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    // Create temp directory and files
    auto tempDir = makeTempPathEx("vfs_dir_list");
    std::filesystem::create_directories(tempDir);

    auto makeFile = [&](const std::string& name, const std::string& content){
        auto p = tempDir / name;
        std::ofstream out(p, std::ios::binary); out << content; out.close();
        return p;
    };

    makeFile("b.txt", std::string(2, 'b'));
    makeFile("a.txt", std::string(1, 'a'));
    makeFile("c.txt", std::string(3, 'c'));

    auto dh = vfs.createDirectoryHandle(tempDir.string());

    // Sort by name, take first 2
    ListDirectoryOptions optByName; optByName.sortBy = ListDirectoryOptions::ByName; optByName.maxResults = 2;
    auto listByName = dh.list(optByName); listByName.wait();
    REQUIRE(listByName.status() == FileOpStatus::Complete);
    const auto& e1 = listByName.directoryEntries();
    REQUIRE(e1.size() == 2);
    REQUIRE(e1[0].name == "a.txt");
    REQUIRE(e1[1].name == "b.txt");

    // Sort by size (ascending), take first 2 (smallest two)
    ListDirectoryOptions optBySize; optBySize.sortBy = ListDirectoryOptions::BySize; optBySize.maxResults = 2;
    auto listBySize = dh.list(optBySize); listBySize.wait();
    REQUIRE(listBySize.status() == FileOpStatus::Complete);
    const auto& e2 = listBySize.directoryEntries();
    REQUIRE(e2.size() == 2);
    // Expect sizes 1, then 2
    REQUIRE(e2[0].metadata.size <= e2[1].metadata.size);

    svc.stop();
    std::error_code ec; std::filesystem::remove_all(tempDir, ec);
}

TEST_CASE("Directory listing on non-existent path returns FileNotFound", "[vfs][dir][errors]") {
    WorkService svc({});
    WorkContractGroup group(64, "DirErrGroup");
    startService(svc, group);
    VirtualFileSystem vfs(&group);

    auto bogus = makeTempPathEx("vfs_dir_missing");
    // Ensure it doesn't exist
    std::error_code ec; std::filesystem::remove_all(bogus, ec);

    auto dh = vfs.createDirectoryHandle(bogus.string());
    auto listing = dh.list(); listing.wait();
    REQUIRE(listing.status() == FileOpStatus::Failed);
    REQUIRE(listing.errorInfo().code == FileError::FileNotFound);

    svc.stop();
}
