/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "PlatformResourceUtils.h"

#include <filesystem>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

namespace EntropyEngine::VirtualFileSystem
{

std::optional<std::string> getExecutablePath() {
#if defined(__APPLE__)
    uint32_t bufferSize = 0;
    _NSGetExecutablePath(nullptr, &bufferSize);

    std::string path(bufferSize, '\0');
    if (_NSGetExecutablePath(path.data(), &bufferSize) != 0) {
        return std::nullopt;
    }

    // Remove null terminator if present
    if (!path.empty() && path.back() == '\0') {
        path.pop_back();
    }

    // Resolve symlinks and normalize
    std::error_code ec;
    auto canonical = std::filesystem::canonical(path, ec);
    if (ec) {
        return path;  // Return non-canonical path if resolution fails
    }
    return canonical.string();

#elif defined(_WIN32)
    char buffer[MAX_PATH];
    DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::nullopt;
    }
    return std::string(buffer, length);

#elif defined(__linux__)
    char buffer[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length == -1) {
        return std::nullopt;
    }
    buffer[length] = '\0';
    return std::string(buffer);

#else
    return std::nullopt;
#endif
}

std::optional<std::string> getExecutableDirectory() {
    auto exePath = getExecutablePath();
    if (!exePath) {
        return std::nullopt;
    }

    std::filesystem::path path(*exePath);
    return path.parent_path().string();
}

std::optional<std::string> getResourcesPath() {
#if defined(__APPLE__)
    // Check if we're running from an app bundle
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (mainBundle) {
        CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
        if (resourcesURL) {
            char buffer[PATH_MAX];
            if (CFURLGetFileSystemRepresentation(resourcesURL, true, reinterpret_cast<UInt8*>(buffer), PATH_MAX)) {
                CFRelease(resourcesURL);
                std::string result(buffer);
                // Ensure trailing slash
                if (!result.empty() && result.back() != '/') {
                    result += '/';
                }
                return result;
            }
            CFRelease(resourcesURL);
        }
    }

    // Fallback: not a bundle, use executable directory
    return getExecutableDirectory();

#else
    // Windows/Linux: resources are next to the executable
    return getExecutableDirectory();
#endif
}

std::optional<std::string> getResourceFilePath(const std::string& relativePath) {
    auto resourcesPath = getResourcesPath();
    if (!resourcesPath) {
        return std::nullopt;
    }

    std::filesystem::path fullPath = std::filesystem::path(*resourcesPath) / relativePath;
    return fullPath.string();
}

}  // namespace EntropyEngine::VirtualFileSystem
