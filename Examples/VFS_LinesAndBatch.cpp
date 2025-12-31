#include <filesystem>
#include <string>

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
    return (dir / (base + ".txt")).string();
}

int main() {
    WorkService svc({});
    WorkContractGroup group(128, "VFS_Lines");
    svc.start();
    svc.addWorkContractGroup(&group);

    VirtualFileSystem vfs(&group);
    auto fh = vfs.createFileHandle(makeTempFile("vfs_lines_batch"));

    // Seed file with two lines (no trailing newline on purpose)
    WriteOptions wo;
    wo.truncate = true;
    wo.ensureFinalNewline = false;
    wo.createIfMissing = true;
    auto w = fh.writeAll("Line one\nLine two", wo);
    w.wait();
    if (w.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR(std::string("Initial write failed: ") + w.errorInfo().message);
        svc.stop();
        return 1;
    }

    // Read the second line (index 1)
    auto r2 = fh.readLine(1);
    r2.wait();
    ENTROPY_LOG_INFO(std::string("Line 2: '") + r2.contentsText() + "'");

    // Replace line 1 (index 0) using writeLine
    auto wl = fh.writeLine(0, "LINE ONE (modified)");
    wl.wait();
    if (wl.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR(std::string("writeLine failed: ") + wl.errorInfo().message);
        svc.stop();
        return 1;
    }

    // Ensure final newline on whole-file rewrite
    WriteOptions wo2;
    wo2.truncate = true;
    wo2.ensureFinalNewline = true;
    auto w2 = fh.writeAll("A single line without LF", wo2);
    w2.wait();
    if (w2.status() != FileOpStatus::Complete) {
        ENTROPY_LOG_ERROR(std::string("ensureFinalNewline write failed: ") + w2.errorInfo().message);
        svc.stop();
        return 1;
    }

    // Verify last byte is a newline by reading all
    auto rAll = fh.readAll();
    rAll.wait();
    auto bytes = rAll.contentsBytes();
    bool endsWithLF = !bytes.empty() && bytes.back() == std::byte('\n');
    ENTROPY_LOG_INFO(std::string("Final newline present: ") + (endsWithLF ? "true" : "false"));

    // Cleanup
    auto rm = fh.remove();
    rm.wait();

    svc.stop();
    return 0;
}
