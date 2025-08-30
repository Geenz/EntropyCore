cmake_minimum_required(VERSION 3.28)
project(EntropyCore 
    VERSION 1.0.0
    DESCRIPTION "Core utilities and concurrency primitives for Entropy Engine"
    LANGUAGES CXX
)

# Options
option(BUILD_SHARED_LIBS "Build as a shared library" OFF)
option(ENTROPY_BUILD_TESTS "Build tests for EntropyCore" OFF)

# Set C++20 standard with modules support
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
#
# # Configure vcpkg
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# Find packages
find_package(Tracy CONFIG REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)

# Core library source files
set(ENTROPY_CORE_SOURCES
        src/Logging/Logger.cpp
        src/Logging/ConsoleSink.cpp
        src/Concurrency/WorkContractHandle.cpp
        src/Concurrency/WorkContractGroup.cpp
        src/Concurrency/WorkGraph.cpp
        src/Concurrency/WorkService.cpp
        src/Concurrency/AdaptiveRankingScheduler.cpp
        src/Concurrency/RandomScheduler.cpp
        src/Concurrency/RoundRobinScheduler.cpp
        src/Concurrency/NodeStateManager.cpp
        src/Concurrency/NodeScheduler.cpp
)

set(ENTROPY_CORE_HEADERS
        src/CoreCommon.h
        src/Core/EventBus.h
        src/EntropyCore.h
        src/ServiceLocator.h
        src/TypeSystem/GenericHandle.h
        src/TypeSystem/TypeID.h
        src/TypeSystem/Reflection.h
        src/Graph/DirectedAcyclicGraph.h
        src/Graph/AcyclicNodeHandle.h
        src/Debug/INamed.h
        src/Debug/DebugUtilities.h
        src/Debug/Profiling.h
        src/Debug/Debug.h
        src/Logging/LogLevel.h
        src/Logging/LogEntry.h
        src/Logging/ILogSink.h
        src/Logging/ConsoleSink.h
        src/Logging/Logger.h
        src/Concurrency/WorkContractHandle.h
        src/Concurrency/WorkContractGroup.h
        src/Concurrency/WorkGraph.h
        src/Concurrency/WorkGraphTypes.h
        src/Concurrency/WorkGraphEvents.h
        src/Concurrency/NodeStateManager.h
        src/Concurrency/NodeScheduler.h
        src/Concurrency/WorkService.h
        src/Concurrency/SignalTree.h
        src/Concurrency/IConcurrencyProvider.h
        src/Concurrency/IWorkScheduler.h
        src/Concurrency/DirectScheduler.h
        src/Concurrency/SpinningDirectScheduler.h
        src/Concurrency/AdaptiveRankingScheduler.h
        src/Concurrency/RandomScheduler.h
        src/Concurrency/RoundRobinScheduler.h
)

# Create the core library
add_library(EntropyCore)
target_sources(EntropyCore
    PRIVATE
        ${ENTROPY_CORE_SOURCES}
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS src
        FILES ${ENTROPY_CORE_HEADERS}
)

# Set target properties for C++20 module support
target_compile_features(EntropyCore PUBLIC cxx_std_20)

# Include directories
target_include_directories(EntropyCore
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

# Link dependencies
target_link_libraries(EntropyCore 
    PUBLIC
        Tracy::TracyClient
)

add_executable(WorkContractExample Examples/WorkContractExample.cpp)

target_link_libraries(WorkContractExample EntropyCore)

# Platform and compiler-specific settings
if(WIN32)
    # Settings for Windows
    target_compile_definitions(EntropyCore PUBLIC WIN32_LEAN_AND_MEAN EntropyWindows)
    if(MSVC)
        target_compile_options(EntropyCore PRIVATE /W4)
    endif()
else()
    # Settings for non-Windows platforms (macOS, Linux)
    target_compile_options(EntropyCore PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(EntropyCore PUBLIC -fexperimental-library)
    if(APPLE)
        # macOS specific
        target_compile_definitions(EntropyCore PUBLIC EntropyDarwin)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Linux specific
        target_compile_definitions(EntropyCore PUBLIC EntropyLinux)
    endif()
endif()

# Define preprocessor macros
target_compile_definitions(EntropyCore
    PUBLIC
        TRACY_ENABLE
        TRACY_ON_DEMAND
)

# Add EntropyDebug macro for Debug and Release with Debug Info builds.
if($<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>)
    target_compile_definitions(EntropyCore PUBLIC EntropyDebug)
endif()

# Export include directories for dependent projects
set_target_properties(EntropyCore PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME Core
)

# Testing
if(ENTROPY_BUILD_TESTS)
    enable_testing()
    
    # Test executable
    add_executable(EntropyCoreTests
            Tests/LoggingTests.cpp
            Tests/DebugInterfaceTests.cpp
            Tests/GraphTests.cpp
            Tests/TypeSystemTests.cpp
            Tests/WorkContractGroupTests.cpp
            Tests/WorkContractHighContentionTest.cpp
            Tests/MainThreadWorkTest.cpp
            Tests/WorkGraphTests.cpp
            Tests/WorkServiceTests.cpp
            Tests/WorkGraphArchitectureTests.cpp
            Tests/WorkGraphServiceIntegrationTests.cpp
            Tests/WorkGraphMemoryTest.cpp
            Tests/WorkGraphDebugTest.cpp
            Tests/WorkGraphSimpleTest.cpp
            Tests/WorkGraphVectorTest.cpp
            Tests/WorkGraphIsolatedHangTest.cpp
            Tests/WorkGraphMainThreadTest.cpp
            Tests/SignalTreeTests.cpp
            Tests/WorkContractGroupAccountingTests.cpp
    )

    target_link_libraries(EntropyCoreTests 
        PRIVATE 
            EntropyCore
            Catch2::Catch2WithMain
    )
    
    target_compile_features(EntropyCoreTests PRIVATE cxx_std_20)
    
    # Register tests with CTest
    include(CTest)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    if(TARGET Catch2::Catch2)
        include(Catch)
        catch_discover_tests(EntropyCoreTests)
    endif()
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS EntropyCore
    EXPORT EntropyCoreTargets
    FILE_SET HEADERS DESTINATION include
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(EXPORT EntropyCoreTargets
    FILE EntropyCoreTargets.cmake
    NAMESPACE EntropyCore::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EntropyCore
)

# Create config file
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "cmake/EntropyCoreConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/EntropyCoreConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EntropyCore
)

write_basic_package_version_file(
    EntropyCoreConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/EntropyCoreConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/EntropyCoreConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EntropyCore
)