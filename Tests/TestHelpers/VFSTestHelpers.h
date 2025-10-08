#pragma once
#include <filesystem>
#include <string>
#include <random>
#include <chrono>

namespace EntropyEngine::Core::Concurrency {
    class WorkService;
    class WorkContractGroup;
}

namespace EntropyTestHelpers {

/**
 * @brief Creates a unique temporary file path with the given base name
 * @param base Base name for the temporary file
 * @return Unique temporary path
 */
std::filesystem::path makeTempPath(const std::string& base = "entropy_test");

/**
 * @brief Extended version with explicit base name parameter
 * @param base Base name for the temporary file
 * @return Unique temporary path
 */
std::filesystem::path makeTempPathEx(const std::string& base);

/**
 * @brief Starts a WorkService and adds a WorkContractGroup to it
 * @param svc WorkService to start
 * @param group WorkContractGroup to add
 */
void startService(EntropyEngine::Core::Concurrency::WorkService& svc,
                 EntropyEngine::Core::Concurrency::WorkContractGroup& group);

/**
 * @brief Reads all bytes from a file
 * @param p Path to file
 * @return File contents as string
 */
std::string readAllBytes(const std::filesystem::path& p);

} // namespace EntropyTestHelpers
