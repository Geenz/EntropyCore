#include <gtest/gtest.h>

#include <string>

#include "VFSTestHelpers.h"
#include "VirtualFileSystem/FileHandle.h"
#include "VirtualFileSystem/VirtualFileSystem.h"

using namespace EntropyEngine::Core::IO;
using entropy::test_helpers::ScopedTempDir;
using entropy::test_helpers::ScopedWorkEnv;

TEST(VFSErrorMapping, ReadNonexistentFile_ReturnsFileNotFound) {
    ScopedWorkEnv env;
    ScopedTempDir tmp;

    auto path = tmp.join("definitely_missing_file.txt").string();
    auto fh = env.vfs().createFileHandle(path);

    auto r = fh.readAll();
    r.wait();
    ASSERT_EQ(r.status(), FileOpStatus::Failed);
    EXPECT_EQ(r.errorInfo().code, FileError::FileNotFound);
}

TEST(VFSErrorMapping, RemoveNonexistentFile_IsIdempotentComplete) {
    ScopedWorkEnv env;
    ScopedTempDir tmp;

    auto path = tmp.join("nothing_here.txt").string();
    auto fh = env.vfs().createFileHandle(path);

    auto rm = fh.remove();
    rm.wait();
    EXPECT_EQ(rm.status(), FileOpStatus::Complete) << rm.errorInfo().message;
}
