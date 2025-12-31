#include <cstring>
#include <filesystem>
#include <vector>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkService.h"
#include "EntropyCore.h"
#include "VirtualFileSystem/FileHandle.h"
#include "VirtualFileSystem/FileStream.h"
#include "VirtualFileSystem/VirtualFileSystem.h"

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

int main() {
    WorkService service({});
    service.start();

    WorkContractGroup group(128, "VFS_Streams");
    service.addWorkContractGroup(&group);

    VirtualFileSystem vfs(&group);
    auto handle = vfs.createFileHandle((std::filesystem::temp_directory_path() / "vfs_streams_demo.txt").string());
    handle.createEmpty().wait();

    // Unbuffered read-write stream
    if (auto stream = handle.openReadWriteStream()) {
        const char* msg = "Streaming API demo\n";
        stream->seek(0, std::ios::end);
        auto wrote = stream->write(std::span<const std::byte>(reinterpret_cast<const std::byte*>(msg), strlen(msg)));
        stream->flush();
        ENTROPY_LOG_INFO(std::string("Unbuffered wrote: ") + std::to_string(wrote.bytesTransferred) + " bytes");

        stream->seek(0, std::ios::beg);
        std::vector<std::byte> buf(256);
        auto read = stream->read(buf);
        ENTROPY_LOG_INFO(std::string("Unbuffered read:  ") + std::to_string(read.bytesTransferred) + " bytes");
    } else {
        ENTROPY_LOG_ERROR("Failed to open unbuffered stream");
    }

    // Buffered stream wrapper
    if (auto base = handle.openReadWriteStream()) {
        BufferedFileStream buffered(std::move(base), 4096);
        const char* msgB = "Buffered block\n";
        buffered.seek(0, std::ios::end);
        auto wroteB =
            buffered.write(std::span<const std::byte>(reinterpret_cast<const std::byte*>(msgB), strlen(msgB)));
        buffered.flush();
        ENTROPY_LOG_INFO(std::string("Buffered wrote:   ") + std::to_string(wroteB.bytesTransferred) + " bytes");

        buffered.seek(0, std::ios::beg);
        std::vector<std::byte> buf2(512);
        auto read2 = buffered.read(buf2);
        ENTROPY_LOG_INFO(std::string("Buffered read:    ") + std::to_string(read2.bytesTransferred) + " bytes");
    }

    service.stop();
    return 0;
}
