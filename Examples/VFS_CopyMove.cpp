#include "EntropyCore.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/FileHandle.h"
#include <filesystem>
#include <string>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

static std::string tempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

int main() {
    WorkService svc({});
    WorkContractGroup group(128, "VFS_CopyMove");
    svc.start();
    svc.addWorkContractGroup(&group);

    VirtualFileSystem vfs(&group);

    auto srcHandle = vfs.createFileHandle(tempPath("vfs_copy_src.txt"));
    auto dstHandle = vfs.createFileHandle(tempPath("vfs_copy_dst.txt"));
    auto dst2Handle = vfs.createFileHandle(tempPath("vfs_move_dst.txt"));

    // Seed source file via FileHandle
    const std::string payload = std::string(4096, 'A');
    WriteOptions wo; wo.truncate = true; wo.createIfMissing = true;
    auto w = srcHandle.writeAll(payload, wo); w.wait();
    if (w.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR(std::string("Seed write failed: ") + w.errorInfo().message);
        svc.stop(); return 1;
    }

    // Copy using FileHandle read/write (simple demo)
    auto r = srcHandle.readAll(); r.wait();
    if (r.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR(std::string("Copy read failed: ") + r.errorInfo().message);
        svc.stop(); return 1;
    }
    WriteOptions cwo; cwo.truncate = true; cwo.createIfMissing = true;
    auto cw = dstHandle.writeAll(r.contentsBytes(), cwo); cw.wait();
    if (cw.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR(std::string("Copy write failed: ") + cw.errorInfo().message);
        svc.stop(); return 1;
    }
    ENTROPY_LOG_INFO(std::string("Copy complete, bytes: ") + std::to_string(cw.bytesWritten()));

    // Move: write to destination (overwrite) then remove source
    auto r2 = dstHandle.readAll(); r2.wait();
    if (r2.status() == FileOpStatus::Complete) {
        WriteOptions mwo; mwo.truncate = true; mwo.createIfMissing = true;
        auto mw = dst2Handle.writeAll(r2.contentsBytes(), mwo); mw.wait();
        if (mw.status() == FileOpStatus::Complete) {
            dstHandle.remove().wait();
            ENTROPY_LOG_INFO(std::string("Move complete: ") + dst2Handle.metadata().path);
        } else {
            ENTROPY_LOG_ERROR(std::string("Move write failed: ") + mw.errorInfo().message);
        }
    } else {
        ENTROPY_LOG_ERROR(std::string("Move read failed: ") + r2.errorInfo().message);
    }

    // Cleanup via handles
    srcHandle.remove().wait();
    dst2Handle.remove().wait();

    svc.stop();
    return 0;
}
