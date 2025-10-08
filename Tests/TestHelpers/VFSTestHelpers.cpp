#include "VFSTestHelpers.h"
#include "Concurrency/WorkService.h"
#include "Concurrency/WorkContractGroup.h"
#include <fstream>
#include <sstream>

namespace EntropyTestHelpers {

std::filesystem::path makeTempPath(const std::string& base) {
    return makeTempPathEx(base);
}

std::filesystem::path makeTempPathEx(const std::string& base) {
    auto dir = std::filesystem::temp_directory_path();
    std::random_device rd;
    for (int i = 0; i < 8; ++i) {
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        auto name = base + "-" + std::to_string(now) + "-" + std::to_string(rd());
        auto p = dir / name;
        if (!std::filesystem::exists(p)) return p;
    }
    return dir / (base + "-" + std::to_string(rd()));
}

void startService(EntropyEngine::Core::Concurrency::WorkService& svc,
                 EntropyEngine::Core::Concurrency::WorkContractGroup& group) {
    svc.start();
    svc.addWorkContractGroup(&group);
}

std::string readAllBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::in | std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace EntropyTestHelpers
