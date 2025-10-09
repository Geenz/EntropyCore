#include "EntropyCore.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/DirectoryHandle.h"
#include "VirtualFileSystem/FileStream.h"
#include <vector>
#include <cstring>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

int main() {
    WorkService service({});
    service.start();

    WorkContractGroup group(256, "VFSStreamGroup");
    service.addWorkContractGroup(&group);

    VirtualFileSystem vfs(&group);

    auto handle = vfs.createFileHandle("vfs_streaming_example.txt");
    handle.createEmpty().wait();

    // 1) Basic unbuffered streaming: append-like write then read back
    if (auto stream = handle.openReadWriteStream()) {
        const char* msg = "Streaming write block A\n";
        stream->seek(0, std::ios::end);
        auto wroteA = stream->write(std::span<const std::byte>(reinterpret_cast<const std::byte*>(msg), strlen(msg)));
        stream->flush();
        ENTROPY_LOG_INFO(std::string("Unbuffered: wrote ") + std::to_string(wroteA.bytesTransferred) + " bytes");

        // Read back entire file
        stream->seek(0, std::ios::beg);
        std::vector<std::byte> buf(1024);
        auto read = stream->read(buf);
        ENTROPY_LOG_INFO(std::string("Unbuffered: read  ") + std::to_string(read.bytesTransferred) + " bytes");
    } else {
        ENTROPY_LOG_ERROR("Failed to open stream");
    }

    // 2) Buffered streaming: write and read using BufferedFileStream
    if (auto base = handle.openReadWriteStream()) {
        BufferedFileStream buffered(std::move(base), 4096);
        const char* msgB = "Buffered streaming block B\n";
        buffered.seek(0, std::ios::end);
        auto wroteB = buffered.write(std::span<const std::byte>(reinterpret_cast<const std::byte*>(msgB), strlen(msgB)));
        buffered.flush();
        ENTROPY_LOG_INFO(std::string("Buffered:   wrote ") + std::to_string(wroteB.bytesTransferred) + " bytes");

        buffered.seek(0, std::ios::beg);
        std::vector<std::byte> buf2(1024);
        auto read2 = buffered.read(buf2);
        ENTROPY_LOG_INFO(std::string("Buffered:   read  ") + std::to_string(read2.bytesTransferred) + " bytes");
    }

    service.stop();
    return 0;
}
