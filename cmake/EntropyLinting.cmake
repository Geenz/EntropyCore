# Enable Linting Control
option(ENTROPY_ENABLE_LINTING "Enable clang-tidy linting" ON)

# Function to enable linting on a target
function(entropy_enable_linting target_name)
    if(ENTROPY_ENABLE_LINTING)
        find_program(CLANG_TIDY_PATH NAMES clang-tidy PATHS
            /opt/homebrew/opt/llvm/bin
            /usr/local/opt/llvm/bin
        )
        if(CLANG_TIDY_PATH)
            message(STATUS "Linting enabled for ${target_name}")
            # MSVC: clang-tidy drops the build's dash-spelled /EHsc and -std:
            # flags, then errors on try/throw. Re-assert them as late extra-args.
            set(_entropy_tidy "${CLANG_TIDY_PATH}")
            if(MSVC)
                list(APPEND _entropy_tidy "--extra-arg=/EHsc" "--extra-arg=/std:c++20")
            endif()
            set_target_properties(${target_name} PROPERTIES
                CXX_CLANG_TIDY "${_entropy_tidy}"
            )
            unset(_entropy_tidy)

            # Add warning flags based on compiler
            if(MSVC)
                target_compile_options(${target_name} PRIVATE /W4 /analyze)
            else()
                target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
            endif()
        else()
            message(WARNING "clang-tidy not found, linting disabled for ${target_name}")
        endif()
    endif()
endfunction()
