# Custom triplet for macOS universal binaries (x86_64 + arm64)
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)

# Build universal binaries with both architectures
set(VCPKG_OSX_ARCHITECTURES "x86_64;arm64")

# Target macOS 13.3 as minimum (required for certain C++ features)
set(VCPKG_OSX_DEPLOYMENT_TARGET "13.3")

# Ensure CMake uses the architectures properly
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")

# Use Release builds for both architectures
if(PORT MATCHES "tracy")
    set(VCPKG_BUILD_TYPE release)
endif()