#include <filesystem>
#include <vector>

#include "Concurrency/WorkContractGroup.h"
#include "Concurrency/WorkService.h"
#include "EntropyCore.h"
#include "VirtualFileSystem/FileHandle.h"
#include "VirtualFileSystem/VirtualFileSystem.h"

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::Concurrency;
using namespace EntropyEngine::Core::IO;

static std::string makeTempFile(const std::string& base) {
    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / (base + ".txt");
    return path.string();
}

int main() {
    // Start work service and group
    WorkService svc({});
    WorkContractGroup group(128, "VFS_Basics");
    svc.start();
    svc.addWorkContractGroup(&group);

    // Create VFS and file handle
    VirtualFileSystem vfs(&group);
    const std::string path = makeTempFile("vfs_basics");
    auto fh = vfs.createFileHandle(path);

    // Write text to a file via FileHandle
    const std::string text = "Hello, VirtualFileSystem!";
    WriteOptions wo;
    wo.truncate = true;
    wo.createIfMissing = true;
    wo.createParentDirs = true;
    auto w = fh.writeAll(text, wo);
    w.wait();
    if (w.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR(std::string("writeAll failed: ") + w.errorInfo().message);
        svc.stop();
        return 1;
    }
    ENTROPY_LOG_INFO(std::string("Wrote: ") + text + " to " + path);

    // Read entire file back via FileHandle
    auto r = fh.readAll();
    r.wait();
    if (r.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR(std::string("readAll failed: ") + r.errorInfo().message);
        svc.stop();
        return 1;
    }
    auto bytes = r.contentsBytes();
    std::string roundTrip(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ENTROPY_LOG_INFO(std::string("Read: ") + roundTrip);

    // Remove the file via FileHandle
    auto rm = fh.remove();
    rm.wait();
    if (rm.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR(std::string("remove failed: ") + rm.errorInfo().message);
        svc.stop();
        return 1;
    }
    ENTROPY_LOG_INFO(std::string("Removed: ") + path);

    svc.stop();
    return 0;
}
