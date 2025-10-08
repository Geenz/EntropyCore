cmake_minimum_required(VERSION 3.28)

# Set vcpkg triplet for macOS universal binaries
if(APPLE AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "universal-osx" CACHE STRING "")
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "")
endif()

project(EntropyCore 
    VERSION 1.0.0
    DESCRIPTION "Core utilities and concurrency primitives for Entropy Engine"
    LANGUAGES CXX
)

# Options
option(BUILD_SHARED_LIBS "Build as a shared library" OFF)
option(ENTROPY_BUILD_TESTS "Build tests for EntropyCore" ON)

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
option(ENTROPY_WITH_TRACY "Enable Tracy profiler integration" ON)
if(ENTROPY_WITH_TRACY)
    find_package(Tracy CONFIG QUIET)
endif()
find_package(Boost REQUIRED COMPONENTS type_index)
find_package(efsw CONFIG REQUIRED)

# Only require Catch2 when building tests
if(ENTROPY_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

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
        src/Core/EntropyObject.cpp
        src/Core/EntropyCAPI.cpp
        src/VirtualFileSystem/VirtualFileSystem.cpp
        src/VirtualFileSystem/FileOperationHandle.cpp
        src/VirtualFileSystem/FileHandle.cpp
        src/VirtualFileSystem/DirectoryHandle.cpp
        src/VirtualFileSystem/LocalFileSystemBackend.cpp
        src/VirtualFileSystem/FileStream.cpp
        src/VirtualFileSystem/WriteBatch.cpp
        src/VirtualFileSystem/FileWatch.cpp
        src/VirtualFileSystem/FileWatchManager.cpp
)

set(ENTROPY_CORE_HEADERS
        src/CoreCommon.h
        src/Core/EventBus.h
        src/Core/EntropyClass.h
        src/Core/EntropyObject.h
        src/Core/RefObject.h
        src/Core/EntropyInterop.h
        src/Core/entropy_c_api.h
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
        src/VirtualFileSystem/VirtualFileSystem.h
        src/VirtualFileSystem/FileOperationHandle.h
        src/VirtualFileSystem/FileHandle.h
        src/VirtualFileSystem/DirectoryHandle.h
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
        Boost::type_index
    PRIVATE
        efsw::efsw
)

# Define for export macros when building the library
target_compile_definitions(EntropyCore PRIVATE ENTROPYCORE_BUILDING)
# Define if building shared library to enable dllexport/dllimport in headers
if(BUILD_SHARED_LIBS)
    target_compile_definitions(EntropyCore PUBLIC ENTROPYCORE_SHARED)
endif()

# Link Tracy if enabled and available
if(ENTROPY_WITH_TRACY)
  if(TARGET Tracy::TracyClient)
    target_link_libraries(EntropyCore PUBLIC Tracy::TracyClient)
  endif()
endif()

add_executable(WorkContractExample Examples/WorkContractExample.cpp)
target_link_libraries(WorkContractExample PRIVATE EntropyCore)

add_executable(WorkGraphYieldableExample Examples/WorkGraphYieldableExample.cpp)
target_link_libraries(WorkGraphYieldableExample PRIVATE EntropyCore)

add_executable(EntropyObjectExample Examples/EntropyObjectExample.cpp)
target_link_libraries(EntropyObjectExample PRIVATE EntropyCore)

add_executable(EntropyObjectHandleExample Examples/EntropyObjectHandleExample.cpp)
target_link_libraries(EntropyObjectHandleExample PRIVATE EntropyCore)

# C ABI example (pure C)
add_executable(CInteropExample Examples/CInteropExample.c Examples/CInteropExample_linkshim.cpp)
# Compile the C example with C++ language to satisfy project language constraints
set_source_files_properties(Examples/CInteropExample.c PROPERTIES LANGUAGE CXX)
# Link against the C++ library; ensure C++ linker is used on some toolchains
set_property(TARGET CInteropExample PROPERTY LINKER_LANGUAGE CXX)
# Link core
target_link_libraries(CInteropExample PRIVATE EntropyCore)

add_executable(VirtualFileSystemExample Examples/VirtualFileSystemExample.cpp)
target_link_libraries(VirtualFileSystemExample PRIVATE EntropyCore)
# Avoid Windows min/max macro pollution
if(WIN32)
  target_compile_definitions(VirtualFileSystemExample PRIVATE NOMINMAX)
endif()

add_executable(VirtualFileStreamingExample Examples/VirtualFileStreamingExample.cpp)
target_link_libraries(VirtualFileStreamingExample PRIVATE EntropyCore)
if(WIN32)
  target_compile_definitions(VirtualFileStreamingExample PRIVATE NOMINMAX)
endif()

# Link Tracy to examples if available
if(ENTROPY_WITH_TRACY)
  if(TARGET Tracy::TracyClient)
    target_link_libraries(WorkContractExample PRIVATE Tracy::TracyClient)
    target_link_libraries(WorkGraphYieldableExample PRIVATE Tracy::TracyClient)
    target_link_libraries(EntropyObjectExample PRIVATE Tracy::TracyClient)
    target_link_libraries(EntropyObjectHandleExample PRIVATE Tracy::TracyClient)
  endif()
endif()

# Platform and compiler-specific settings
if(WIN32)
    # Settings for Windows
    target_compile_definitions(EntropyCore PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX EntropyWindows)
    if(MSVC)
        target_compile_options(EntropyCore PRIVATE /W4)
        # Use static runtime library to match vcpkg dependencies
        set_property(TARGET EntropyCore PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        set_property(TARGET WorkContractExample PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        set_property(TARGET WorkGraphYieldableExample PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        set_property(TARGET EntropyObjectExample PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        set_property(TARGET EntropyObjectHandleExample PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        set_property(TARGET CInteropExample PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        set_property(TARGET VirtualFileSystemExample PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        set_property(TARGET VirtualFileStreamingExample PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
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
if(ENTROPY_WITH_TRACY AND TARGET Tracy::TracyClient)
    target_compile_definitions(EntropyCore
        PUBLIC
            TRACY_ENABLE
            TRACY_ON_DEMAND
    )
endif()

# Add EntropyDebug macro for Debug and RelWithDebInfo builds via generator expression
target_compile_definitions(EntropyCore PUBLIC $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:EntropyDebug>)

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
    # Test helper sources
    set(TEST_HELPERS
            Tests/TestHelpers/VFSTestHelpers.cpp
    )

    # Core utility tests
    set(CORE_TESTS
            Tests/Core/LoggingTests.cpp
            Tests/Core/DebugInterfaceTests.cpp
            Tests/Core/GraphTests.cpp
            Tests/Core/TypeSystemTests.cpp
            Tests/Core/SignalTreeTests.cpp
            Tests/Core/EntropyObjectTests.cpp
    )

    # Concurrency tests
    set(CONCURRENCY_TESTS
            Tests/Concurrency/WorkContractGroupTests.cpp
            Tests/Concurrency/WorkContractGroupAccountingTests.cpp
            Tests/Concurrency/WorkContractGroupLifetimeTests.cpp
            Tests/Concurrency/WorkContractHighContentionTests.cpp
            Tests/Concurrency/MainThreadWorkTests.cpp
            Tests/Concurrency/WorkServiceTests.cpp
    )

    # WorkGraph tests
    set(WORKGRAPH_TESTS
            Tests/Concurrency/WorkGraph/WorkGraphTests.cpp
            Tests/Concurrency/WorkGraph/WorkGraphArchitectureTests.cpp
            Tests/Concurrency/WorkGraph/WorkGraphServiceIntegrationTests.cpp
            Tests/Concurrency/WorkGraph/WorkGraphRegressionTests.cpp
            Tests/Concurrency/WorkGraph/WorkGraphSpecializedTests.cpp
    )

    # VirtualFileSystem tests
    set(VFS_TESTS
            Tests/VirtualFileSystem/VFSTests.cpp
            Tests/VirtualFileSystem/VFSTextFidelityTests.cpp
            Tests/VirtualFileSystem/VFSErrorMappingTests.cpp
    )

    add_executable(EntropyCoreTests
            ${TEST_HELPERS}
            ${CORE_TESTS}
            ${CONCURRENCY_TESTS}
            ${WORKGRAPH_TESTS}
            ${VFS_TESTS}
    )

    target_link_libraries(EntropyCoreTests 
        PRIVATE 
            EntropyCore
            Catch2::Catch2WithMain
    )
    
    target_compile_features(EntropyCoreTests PRIVATE cxx_std_20)

    if(WIN32 AND MSVC)
        set_property(TARGET EntropyCoreTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    
    # Register tests with CTest
    include(CTest)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    if(TARGET Catch2::Catch2)
        include(Catch)
        catch_discover_tests(EntropyCoreTests)
    endif()

    # Convenience target: build tests and run them in one step. This prevents running stale binaries
    # because the test execution only happens if EntropyCoreTests builds successfully.
    add_custom_target(RunAllTests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
        DEPENDS EntropyCoreTests
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
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