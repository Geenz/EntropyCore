#include <catch2/catch_test_macros.hpp>
#include "EntropyCore.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include <vector>
#include <cstring>
#include <fstream>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

static void setup_service(WorkService& svc, WorkContractGroup& group) {
    svc.start();
    svc.addWorkContractGroup(&group);
}

TEST_CASE("VFS streaming unbuffered roundtrip", "[vfs][stream]") {
    WorkService svc({});
    WorkContractGroup group(128, "TestGroup");
    setup_service(svc, group);
    VirtualFileSystem vfs(&group);

    auto fh = vfs.createFileHandle("vfs_test_stream_unbuffered.txt");
    fh.createEmpty().wait();

    auto stream = fh.openReadWriteStream();
    REQUIRE(stream);

    const char* msg = "Hello via stream\n";
    stream->seek(0, std::ios::end);
    auto wrote = stream->write(std::span<const std::byte>(reinterpret_cast<const std::byte*>(msg), strlen(msg)));
    stream->flush();
    REQUIRE(wrote.bytesTransferred == strlen(msg));

    stream->seek(0, std::ios::beg);
    std::vector<std::byte> buf(64);
    auto read = stream->read(buf);
    REQUIRE(read.bytesTransferred >= wrote.bytesTransferred);

    svc.stop();
}

TEST_CASE("VFS streaming buffered roundtrip", "[vfs][stream][buffered]") {
    WorkService svc({});
    WorkContractGroup group(128, "TestGroup");
    setup_service(svc, group);
    VirtualFileSystem vfs(&group);

    auto fh = vfs.createFileHandle("vfs_test_stream_buffered.txt");
    fh.createEmpty().wait();

    auto base = fh.openReadWriteStream();
    REQUIRE(base);
    BufferedFileStream buffered(std::move(base), 8192);

    const char* msg = "Buffered hello\n";
    buffered.seek(0, std::ios::end);
    auto wrote = buffered.write(std::span<const std::byte>(reinterpret_cast<const std::byte*>(msg), strlen(msg)));
    buffered.flush();
    REQUIRE(wrote.bytesTransferred == strlen(msg));

    buffered.seek(0, std::ios::beg);
    std::vector<std::byte> buf(64);
    auto read = buffered.read(buf);
    REQUIRE(read.bytesTransferred >= wrote.bytesTransferred);

    svc.stop();
}

TEST_CASE("VFS readLine trims CRLF and writeLine atomic replace", "[vfs][line]") {
    WorkService svc({});
    WorkContractGroup group(128, "TestGroup");
    setup_service(svc, group);
    VirtualFileSystem vfs(&group);

    auto fh = vfs.createFileHandle("vfs_test_lines.txt");
    auto w = fh.writeAll("Line1\r\nLine2\r\n");
    w.wait();
    REQUIRE(w.status() == FileOpStatus::Complete);

    auto l1 = fh.readLine(0);
    l1.wait();
    REQUIRE(l1.status() == FileOpStatus::Complete);
    REQUIRE(l1.contentsText() == std::string("Line1"));

    auto wl = fh.writeLine(1, "ReplacedLine2");
    wl.wait();
    REQUIRE(wl.status() == FileOpStatus::Complete);

    auto l2 = fh.readLine(1);
    l2.wait();
    REQUIRE(l2.status() == FileOpStatus::Complete);
    REQUIRE(l2.contentsText() == std::string("ReplacedLine2"));

    svc.stop();
}
