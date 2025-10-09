#include "EntropyCore.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/FileHandle.h"
#include "VirtualFileSystem/DirectoryHandle.h"
#include <filesystem>
#include <vector>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

static std::string tp(const std::string& name) { return (std::filesystem::temp_directory_path() / name).string(); }

int main() {
    WorkService svc({});
    WorkContractGroup group(128, "VFS_Metadata");
    svc.start(); svc.addWorkContractGroup(&group);

    VirtualFileSystem vfs(&group);

    // Prepare a couple of files using FileHandle
    auto fh1 = vfs.createFileHandle(tp("vfs_meta_1.txt"));
    auto fh2 = vfs.createFileHandle(tp("vfs_meta_2.txt"));
    const std::string data = "metadata demo";
    WriteOptions wo; wo.truncate = true; wo.createIfMissing = true;
    fh1.writeAll(data, wo).wait();
    fh2.writeAll(data, wo).wait();

    // Single file metadata via DirectoryHandle listing with glob
    auto tempDir = vfs.createDirectoryHandle(std::filesystem::temp_directory_path().string());
    ListDirectoryOptions single; single.globPattern = std::filesystem::path(fh1.metadata().path).filename().string();
    auto singleList = tempDir.list(single); singleList.wait();
    if (singleList.status() == FileOpStatus::Complete) {
        const auto& entries = singleList.directoryEntries();
        if (!entries.empty()) {
            const auto& meta = entries.front().metadata;
            ENTROPY_LOG_INFO(entries.front().fullPath + ": size=" + std::to_string(meta.size) + ", exists=" + std::to_string(meta.exists ? 1 : 0));
        }
    }

    // Batch-like metadata via listing of temp directory with glob
    ListDirectoryOptions batch; batch.globPattern = std::string("vfs_meta_*.txt"); batch.sortBy = ListDirectoryOptions::ByName;
    auto batchList = tempDir.list(batch); batchList.wait();
    if (batchList.status() == FileOpStatus::Complete) {
        for (const auto& e : batchList.directoryEntries()) {
            ENTROPY_LOG_INFO(e.fullPath + ": exists=" + std::to_string(e.metadata.exists ? 1 : 0) + ", size=" + std::to_string(e.metadata.size));
        }
    }

    // Cleanup via FileHandle
    fh1.remove().wait();
    fh2.remove().wait();

    svc.stop();
    return 0;
}
