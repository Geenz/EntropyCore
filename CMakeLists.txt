cmake_minimum_required(VERSION 3.28)

project(EntropyCore 
    VERSION 1.0.0
    DESCRIPTION "Core utilities and concurrency primitives for Entropy Engine"
    LANGUAGES C CXX
)

# Options
# EntropyCore MUST ALWAYS be built as a static library - shared linking is not supported
set(BUILD_SHARED_LIBS OFF CACHE BOOL "EntropyCore only supports static linking" FORCE)
# New unified toggle for tests. Prefer this over ENTROPY_BUILD_TESTS.
option(ENTROPY_ENABLE_TESTS "Enable/build tests for EntropyCore" OFF)
# Make building examples optional (disabled by default, e.g., for CI/CD)
option(ENTROPY_BUILD_EXAMPLES "Build EntropyCore example executables" OFF)
# Entropy does not depend on C++ RTTI. You may optionally disable RTTI for these targets if desired.
option(ENTROPY_REQUIRE_NO_RTTI "Disable C++ RTTI for Entropy targets" OFF)
# Enable sanitizers for debugging and testing
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" ON)

# Default behavior: on single-config generators, enable tests in Debug/RelWithDebInfo
# No build-dir heuristics; on multi-config generators we do not auto-enable by default.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT DEFINED CACHE{ENTROPY_ENABLE_TESTS})
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(ENTROPY_ENABLE_TESTS ON CACHE BOOL "Enable/build tests for EntropyCore" FORCE)
    endif()
endif()

# Back-compat shim: mirror to existing variable used throughout the file
set(ENTROPY_BUILD_TESTS ${ENTROPY_ENABLE_TESTS} CACHE BOOL "Build tests for EntropyCore (deprecated; use ENTROPY_ENABLE_TESTS)" FORCE)

# Set C++20 standard with modules support
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Configure sanitizers
if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # ThreadSanitizer is mutually exclusive with AddressSanitizer
        # To use TSAN instead: cmake -DENABLE_TSAN=ON
        if(ENABLE_TSAN)
            set(SANITIZER_FLAGS "-fsanitize=thread -fno-omit-frame-pointer -g")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
            message(STATUS "ThreadSanitizer (TSAN) enabled")
        else()
            set(SANITIZER_FLAGS "-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -g")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
            message(STATUS "Sanitizers enabled: AddressSanitizer (ASAN) and UndefinedBehaviorSanitizer (UBSAN)")
        endif()
    else()
        message(WARNING "Sanitizers requested but not supported by ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

#
# # Configure vcpkg
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# Find packages
find_package(Boost REQUIRED COMPONENTS type_index)
find_package(efsw CONFIG REQUIRED)

# Only require GoogleTest when building tests
if(ENTROPY_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

# Core library source files
set(ENTROPY_CORE_SOURCES
        src/Logging/Logger.cpp
        src/Logging/ConsoleSink.cpp
        src/Logging/CLogger.cpp
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
        src/Core/EntropyServiceRegistry.cpp
        src/Core/EntropyApplication.cpp
        src/Core/EntropyMain.cpp
        src/Core/Timer.cpp
        src/Core/TimerService.cpp
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
        src/Core/EntropyService.h
        src/Core/EntropyServiceRegistry.h
        src/Core/EntropyApplication.h
        src/Core/entropy_main.h
        src/Core/Timer.h
        src/Core/TimerService.h
        src/TypeSystem/GenericHandle.h
        src/TypeSystem/TypeID.h
        src/TypeSystem/Reflection.h
        src/Graph/DirectedAcyclicGraph.h
        src/Graph/AcyclicNodeHandle.h
        src/Debug/INamed.h
        src/Debug/DebugUtilities.h
        src/Debug/Debug.h
        src/Logging/LogLevel.h
        src/Logging/LogEntry.h
        src/Logging/ILogSink.h
        src/Logging/ConsoleSink.h
        src/Logging/Logger.h
        src/Logging/CLogger.h
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
        src/VirtualFileSystem/IFileSystemBackend.h
        src/VirtualFileSystem/LocalFileSystemBackend.h
        src/VirtualFileSystem/FileOperationHandle.h
        src/VirtualFileSystem/FileHandle.h
        src/VirtualFileSystem/DirectoryHandle.h
        src/VirtualFileSystem/FileStream.h
        src/VirtualFileSystem/WriteBatch.h
        src/VirtualFileSystem/FileWatch.h
        src/VirtualFileSystem/FileWatchManager.h
)

# C Concurrency API sources and headers
set(ENTROPY_C_API_SOURCES
        src/entropy/entropy_concurrency_types_c.cpp
        src/entropy/entropy_work_contract_handle_c.cpp
        src/entropy/entropy_work_contract_group_c.cpp
        src/entropy/entropy_work_service_c.cpp
        src/entropy/entropy_work_graph_c.cpp
)

set(ENTROPY_C_API_HEADERS
        include/entropy/entropy_concurrency_types.h
        include/entropy/entropy_work_contract_handle.h
        include/entropy/entropy_work_contract_group.h
        include/entropy/entropy_work_service.h
        include/entropy/entropy_work_graph.h
)

# VFS C API sources and headers
set(ENTROPY_VFS_C_API_SOURCES
        src/entropy/entropy_vfs_types_c.cpp
        src/entropy/entropy_virtual_file_system_c.cpp
        src/entropy/entropy_file_handle_c.cpp
        src/entropy/entropy_directory_handle_c.cpp
        src/entropy/entropy_file_operation_handle_c.cpp
        src/entropy/entropy_write_batch_c.cpp
)

set(ENTROPY_VFS_C_API_HEADERS
        include/entropy/entropy_vfs_types.h
        include/entropy/entropy_virtual_file_system.h
        include/entropy/entropy_file_handle.h
        include/entropy/entropy_directory_handle.h
        include/entropy/entropy_file_operation_handle.h
        include/entropy/entropy_write_batch.h
)

# Append to main lists
list(APPEND ENTROPY_C_API_SOURCES ${ENTROPY_VFS_C_API_SOURCES})
list(APPEND ENTROPY_C_API_HEADERS ${ENTROPY_VFS_C_API_HEADERS})
list(APPEND ENTROPY_CORE_SOURCES ${ENTROPY_C_API_SOURCES})
list(APPEND ENTROPY_CORE_HEADERS ${ENTROPY_C_API_HEADERS})

# Create the core library (STATIC only - shared libs are not supported)
add_library(EntropyCore STATIC)
target_sources(EntropyCore
    PRIVATE
        ${ENTROPY_CORE_SOURCES}
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS src include
        FILES ${ENTROPY_CORE_HEADERS}
)

# Set target properties for C++20 module support
target_compile_features(EntropyCore PUBLIC cxx_std_20)

# Include directories
target_include_directories(EntropyCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
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
# EntropyCore is always static - no ENTROPYCORE_SHARED definition needed

if(ENTROPY_BUILD_EXAMPLES)
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
  # Link against the C++ library; ensure C++ linker is used on some toolchains
  set_property(TARGET CInteropExample PROPERTY LINKER_LANGUAGE CXX)
  # Link core
  target_link_libraries(CInteropExample PRIVATE EntropyCore)

  # C Concurrency API example (pure C)
  add_executable(ConcurrencyAPIExample Examples/ConcurrencyAPIExample.c)
  # Ensure C++ linker is used (C target linking against C++ library)
  set_property(TARGET ConcurrencyAPIExample PROPERTY LINKER_LANGUAGE CXX)
  # Link core
  target_link_libraries(ConcurrencyAPIExample PRIVATE EntropyCore)

  # VFS C API example (pure C)
  add_executable(VFSCAPIExample Examples/VFSCAPIExample.c)
  # Ensure C++ linker is used (C target linking against C++ library)
  set_property(TARGET VFSCAPIExample PROPERTY LINKER_LANGUAGE CXX)
  # Link core
  target_link_libraries(VFSCAPIExample PRIVATE EntropyCore)

  # WorkGraph C API example (pure C)
  add_executable(WorkGraphCAPIExample Examples/WorkGraphCAPIExample.c)
  # Ensure C++ linker is used (C target linking against C++ library)
  set_property(TARGET WorkGraphCAPIExample PROPERTY LINKER_LANGUAGE CXX)
  # Link core
  target_link_libraries(WorkGraphCAPIExample PRIVATE EntropyCore)

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

  # Focused VFS examples (PR3)
  add_executable(VFS_Basics Examples/VFS_Basics.cpp)
  target_link_libraries(VFS_Basics PRIVATE EntropyCore)

  add_executable(VFS_LinesAndBatch Examples/VFS_LinesAndBatch.cpp)
  target_link_libraries(VFS_LinesAndBatch PRIVATE EntropyCore)

  add_executable(VFS_Streams Examples/VFS_Streams.cpp)
  target_link_libraries(VFS_Streams PRIVATE EntropyCore)

  add_executable(VFS_CopyMove Examples/VFS_CopyMove.cpp)
  target_link_libraries(VFS_CopyMove PRIVATE EntropyCore)

  add_executable(VFS_Metadata Examples/VFS_Metadata.cpp)
  target_link_libraries(VFS_Metadata PRIVATE EntropyCore)
  # EntropyMain C ABI example
  add_executable(EntropyMainExample Examples/EntropyMainExample.c)
  # Ensure C++ linker is used (C target linking against C++ library)
  set_property(TARGET EntropyMainExample PROPERTY LINKER_LANGUAGE CXX)
  # Link core
  target_link_libraries(EntropyMainExample PRIVATE EntropyCore)

  # C++ EntropyApplication/Delegate example
  add_executable(EntropyCppAppExample Examples/EntropyCppAppExample.cpp)
  target_link_libraries(EntropyCppAppExample PRIVATE EntropyCore)

  # Custom AppDelegate example with main thread work pump
  add_executable(CustomAppDelegateExample Examples/CustomAppDelegateExample.cpp)
  target_link_libraries(CustomAppDelegateExample PRIVATE EntropyCore)
endif()

# Platform and compiler-specific settings
if(WIN32)
    # Settings for Windows
    target_compile_definitions(EntropyCore PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX EntropyWindows)
    if(MSVC)
        target_compile_options(EntropyCore PRIVATE /W4)
        # Optionally disable RTTI if requested
        if(ENTROPY_REQUIRE_NO_RTTI)
            target_compile_options(EntropyCore PRIVATE /GR-)
            foreach(tgt IN ITEMS WorkContractExample WorkGraphYieldableExample EntropyObjectExample EntropyObjectHandleExample CInteropExample EntropyMainExample EntropyCppAppExample CustomAppDelegateExample WorkGraphCAPIExample)
                if(TARGET ${tgt})
                    target_compile_options(${tgt} PRIVATE /GR-)
                endif()
            endforeach()
        endif()
    endif()
else()
    # Settings for non-Windows platforms (macOS, Linux)
    target_compile_options(EntropyCore PRIVATE -Wall -Wextra -Wpedantic)
    # -fexperimental-library is only supported by Clang, not GCC
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(EntropyCore PUBLIC -fexperimental-library)
    endif()
    if(APPLE)
        # macOS specific
        target_compile_definitions(EntropyCore PUBLIC EntropyDarwin)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Linux specific
        target_compile_definitions(EntropyCore PUBLIC EntropyLinux)
    endif()
    # Optionally disable RTTI if requested
    if(ENTROPY_REQUIRE_NO_RTTI)
        target_compile_options(EntropyCore PRIVATE -fno-rtti)
        foreach(tgt IN ITEMS WorkContractExample WorkGraphYieldableExample EntropyObjectExample EntropyObjectHandleExample CInteropExample EntropyMainExample EntropyCppAppExample CustomAppDelegateExample WorkGraphCAPIExample)
            if(TARGET ${tgt})
                target_compile_options(${tgt} PRIVATE -fno-rtti)
            endif()
        endforeach()
    endif()
endif()

# Define preprocessor macros
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
            Tests/Core/TimerTests.cpp
    )

    # Concurrency tests
    set(CONCURRENCY_TESTS
            Tests/Concurrency/WorkContractGroupTests.cpp
            Tests/Concurrency/WorkContractGroupAccountingTests.cpp
            Tests/Concurrency/WorkContractGroupLifetimeTests.cpp
            Tests/Concurrency/WorkContractGroupReentranceTests.cpp
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
            Tests/VirtualFileSystem/VFSLockCacheTests.cpp
    )

    # Simple diagnostic test (standalone executable, not using GTest)
    add_executable(VFSSimpleTest Tests/VirtualFileSystem/VFSSimpleTest.cpp Tests/TestHelpers/VFSTestHelpers.cpp)
    target_include_directories(VFSSimpleTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TestHelpers ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(VFSSimpleTest PRIVATE EntropyCore)
    target_compile_features(VFSSimpleTest PRIVATE cxx_std_20)

    # Minimal GTest test without gtest_main to diagnose hanging
    add_executable(MinimalGTestNoMain Tests/MinimalGTestNoMain.cpp)
    target_include_directories(MinimalGTestNoMain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(MinimalGTestNoMain PRIVATE EntropyCore GTest::gtest GTest::gtest_main)
    target_compile_features(MinimalGTestNoMain PRIVATE cxx_std_20)

    # Create single test executable with all test sources (like EntropyNetworking)
    add_executable(EntropyCoreTests
            ${TEST_HELPERS}
            ${CORE_TESTS}
            ${CONCURRENCY_TESTS}
            ${WORKGRAPH_TESTS}
            ${VFS_TESTS}
    )

    target_include_directories(EntropyCoreTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TestHelpers
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(EntropyCoreTests PRIVATE
        EntropyCore
        GTest::gtest
        GTest::gtest_main
    )

    target_compile_features(EntropyCoreTests PRIVATE cxx_std_20)

    # Enable testing and discover tests automatically
    include(GoogleTest)

    # Discover tests with sane defaults for hang detection
    gtest_discover_tests(EntropyCoreTests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 90
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