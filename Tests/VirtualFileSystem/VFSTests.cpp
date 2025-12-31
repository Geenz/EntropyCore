#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "EntropyCore.h"
#include "VFSTestHelpers.h"
#include "VirtualFileSystem/DirectoryHandle.h"
#include "VirtualFileSystem/FileHandle.h"
#include "VirtualFileSystem/VirtualFileSystem.h"

using namespace EntropyEngine::Core;
using namespace EntropyEngine::Core::IO;
using entropy::test_helpers::ScopedTempDir;
using entropy::test_helpers::ScopedWorkEnv;

TEST(VFS, WriteReadDelete_InTempDir) {
    ScopedWorkEnv env;
    ScopedTempDir tmp;

    auto filePath = tmp.join("vfs_test_file.txt");
    auto fh = env.vfs().createFileHandle(filePath.string());

    const std::string text = "Hello from VFSTests!\n";

    // Write
    auto w = fh.writeAll(std::string_view(text));
    w.wait();
    ASSERT_EQ(w.status(), FileOpStatus::Complete) << "writeAll failed: " << w.errorInfo().message;
    EXPECT_EQ(w.bytesWritten(), static_cast<uint64_t>(text.size()));

    // Read back
    auto r = fh.readAll();
    r.wait();
    ASSERT_EQ(r.status(), FileOpStatus::Complete) << "readAll failed: " << r.errorInfo().message;
    const auto roundTrip = r.contentsText();
    EXPECT_NE(roundTrip.find("Hello"), std::string::npos);

    // Remove
    auto rem = fh.remove();
    rem.wait();
    ASSERT_EQ(rem.status(), FileOpStatus::Complete) << "remove failed: " << rem.errorInfo().message;
}

TEST(VFS, ListTempDir_ShowsCreatedFile) {
    ScopedWorkEnv env;
    ScopedTempDir tmp;

    auto filePath = tmp.join("file.txt");
    auto fh = env.vfs().createFileHandle(filePath.string());

    auto w = fh.writeAll(std::string_view("x"));
    w.wait();
    ASSERT_EQ(w.status(), FileOpStatus::Complete) << "writeAll failed: " << w.errorInfo().message;

    auto dh = env.vfs().createDirectoryHandle(tmp.path().string());
    auto list = dh.list();
    list.wait();
    ASSERT_EQ(list.status(), FileOpStatus::Complete) << "list failed: " << list.errorInfo().message;

    const auto& entries = list.directoryEntries();
    const bool found = std::any_of(entries.begin(), entries.end(), [](const auto& e) { return e.name == "file.txt"; });
    EXPECT_TRUE(found) << "Expected to find file.txt in temp dir listing";
}
