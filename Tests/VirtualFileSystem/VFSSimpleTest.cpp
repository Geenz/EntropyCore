#include "EntropyCore.h"
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/FileHandle.h"
#include "../TestHelpers/VFSTestHelpers.h"
#include <iostream>

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::IO;
using entropy::test_helpers::ScopedWorkEnv;
using entropy::test_helpers::ScopedTempDir;

int main() {
    std::cout << "Starting VFS simple test...\n";

    ScopedWorkEnv env;
    std::cout << "Created ScopedWorkEnv\n";

    ScopedTempDir tmp;
    std::cout << "Created ScopedTempDir\n";

    auto filePath = tmp.join("simple_test_file.txt");
    std::cout << "Test file path: " << filePath << "\n";

    auto fh = env.vfs().createFileHandle(filePath.string());
    std::cout << "Created file handle\n";

    // Write
    std::cout << "Calling writeAll...\n";
    auto w = fh.writeAll(std::string_view("Hello from simple test!\n"));
    std::cout << "Waiting for write...\n";
    w.wait();
    std::cout << "Write status: " << static_cast<int>(w.status()) << "\n";

    if (w.status() == FileOpStatus::Complete) {
        std::cout << "SUCCESS: Write completed\n";

        // Read back
        std::cout << "Calling readAll...\n";
        auto r = fh.readAll();
        std::cout << "Waiting for read...\n";
        r.wait();
        std::cout << "Read status: " << static_cast<int>(r.status()) << "\n";

        if (r.status() == FileOpStatus::Complete) {
            std::cout << "SUCCESS: Read completed\n";
            std::cout << "Content: " << r.contentsText() << "\n";
            return 0;
        }
    }

    std::cout << "FAILED\n";
    return 1;
}
