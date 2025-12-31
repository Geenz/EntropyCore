#pragma once
#include <chrono>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>

#include "EntropyCore.h"
#include "VirtualFileSystem/VirtualFileSystem.h"

namespace entropy::test_helpers
{

// RAII temporary directory that gets cleaned up on destruction
class ScopedTempDir
{
public:
    ScopedTempDir() {
        namespace fs = std::filesystem;
        auto base = fs::temp_directory_path();
        // Create a reasonably unique directory name
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::random_device rd;
        std::mt19937_64 gen(rd());
        auto rnd = gen();
        std::ostringstream oss;
        oss << "EntropyVFS_Test_" << std::hex << now << "_" << rnd;
        _path = base / oss.str();
        std::error_code ec;
        fs::create_directories(_path, ec);
    }

    ~ScopedTempDir() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove_all(_path, ec);  // best-effort cleanup
    }

    const std::filesystem::path& path() const noexcept {
        return _path;
    }
    std::filesystem::path join(const std::string& name) const {
        return _path / name;
    }

private:
    std::filesystem::path _path;
};

// RAII environment with a running WorkService, a WorkContractGroup and a VFS instance
class ScopedWorkEnv
{
public:
    ScopedWorkEnv()
        : _service(EntropyEngine::Core::Concurrency::WorkService::Config{}),
          _group(2048, "TestVFSGroup"),
          _vfs(&_group, EntropyEngine::Core::IO::VirtualFileSystem::Config{}) {
        _service.start();
        _service.addWorkContractGroup(&_group);
    }

    ~ScopedWorkEnv() {
        // Attempt graceful teardown
        _service.removeWorkContractGroup(&_group);
        _service.stop();
    }

    EntropyEngine::Core::IO::VirtualFileSystem& vfs() noexcept {
        return _vfs;
    }
    EntropyEngine::Core::Concurrency::WorkContractGroup& group() noexcept {
        return _group;
    }

private:
    EntropyEngine::Core::Concurrency::WorkService _service;
    EntropyEngine::Core::Concurrency::WorkContractGroup _group;
    EntropyEngine::Core::IO::VirtualFileSystem _vfs;
};

}  // namespace entropy::test_helpers
