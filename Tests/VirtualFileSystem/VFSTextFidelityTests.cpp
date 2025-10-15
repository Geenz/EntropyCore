#include <gtest/gtest.h>
#include <string>
#include "VirtualFileSystem/VirtualFileSystem.h"
#include "VirtualFileSystem/FileHandle.h"
#include "VirtualFileSystem/WriteBatch.h"
#include "VFSTestHelpers.h"

using namespace EntropyEngine::Core::IO;
using entropy::test_helpers::ScopedWorkEnv;
using entropy::test_helpers::ScopedTempDir;

static std::string readLineText(const FileOperationHandle& h) {
    auto bytes = h.contentsBytes();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

TEST(VFSTextFidelity, AppendAndReadLines_PreservesOrder) {
    ScopedWorkEnv env;
    ScopedTempDir tmp;

    auto path = tmp.join("text_fidelity.txt").string();
    VirtualFileSystem& vfs = env.vfs();

    auto batch = vfs.createWriteBatch(path);
    batch->appendLine("alpha").appendLine("beta").appendLine("gamma");
    auto c = batch->commit();
    c.wait();
    ASSERT_EQ(c.status(), FileOpStatus::Complete) << c.errorInfo().message;

    auto fh = vfs.createFileHandle(path);
    auto l0 = fh.readLine(0); l0.wait(); ASSERT_EQ(l0.status(), FileOpStatus::Complete);
    auto l1 = fh.readLine(1); l1.wait(); ASSERT_EQ(l1.status(), FileOpStatus::Complete);
    auto l2 = fh.readLine(2); l2.wait(); ASSERT_EQ(l2.status(), FileOpStatus::Complete);

    EXPECT_EQ(readLineText(l0), "alpha");
    EXPECT_EQ(readLineText(l1), "beta");
    EXPECT_EQ(readLineText(l2), "gamma");
}

TEST(VFSTextFidelity, WriteLine_BeyondEOF_ExtendsAndWrites) {
    ScopedWorkEnv env;
    ScopedTempDir tmp;

    auto path = tmp.join("extend_lines.txt").string();
    VirtualFileSystem& vfs = env.vfs();
    auto fh = vfs.createFileHandle(path);

    // Start with one line
    auto w0 = fh.writeAll("root\n");
    w0.wait();
    ASSERT_EQ(w0.status(), FileOpStatus::Complete);

    // Write line 5 (0-based), which should extend file with blank lines
    auto wl = fh.writeLine(5, "delta");
    wl.wait();
    ASSERT_EQ(wl.status(), FileOpStatus::Complete) << wl.errorInfo().message;

    // The written line should be readable exactly
    auto r = fh.readLine(5); r.wait();
    ASSERT_EQ(r.status(), FileOpStatus::Complete);
    EXPECT_EQ(readLineText(r), std::string("delta"));
}
