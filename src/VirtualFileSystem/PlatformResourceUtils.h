/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <optional>
#include <string>

namespace EntropyEngine::VirtualFileSystem
{

/**
 * Get the application's resources directory (cross-platform).
 *
 * Platform behavior:
 * - macOS App Bundle: Returns MyApp.app/Contents/Resources/
 * - macOS CLI/Windows/Linux: Returns directory containing the executable
 *
 * @return Absolute path to resources directory, or nullopt on failure
 */
std::optional<std::string> getResourcesPath();

/**
 * Get the full path to a resource file.
 *
 * @param relativePath Path relative to resources directory (e.g., "Shaders/blit.slang")
 * @return Absolute path to the resource file, or nullopt on failure
 */
std::optional<std::string> getResourceFilePath(const std::string& relativePath);

/**
 * Get the directory containing the executable.
 *
 * @return Absolute path to executable's directory, or nullopt on failure
 */
std::optional<std::string> getExecutableDirectory();

/**
 * Get the full path to the executable.
 *
 * @return Absolute path to executable, or nullopt on failure
 */
std::optional<std::string> getExecutablePath();

}  // namespace EntropyEngine::VirtualFileSystem
