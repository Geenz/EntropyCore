#!/bin/bash

# Run clang-tidy
# Usage: ./tools/lint_code.sh [build_dir]

BUILD_DIR="build"
if [ "$#" -gt 0 ]; then
    BUILD_DIR="$1"
fi

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    echo "Error: compile_commands.json not found in $BUILD_DIR"
    echo "Please configure the project with CMAKE_EXPORT_COMPILE_COMMANDS=ON first."
    echo "Example: cmake -S . -B $BUILD_DIR -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    exit 1
fi

echo "Running clang-tidy using build directory: $BUILD_DIR"

# Check for clang-tidy
if ! command -v clang-tidy &> /dev/null; then
    # Try finding it in brew paths if not in PATH
    if [ -f "/opt/homebrew/opt/llvm/bin/clang-tidy" ]; then
        CLANG_TIDY="/opt/homebrew/opt/llvm/bin/clang-tidy"
    elif [ -f "/usr/local/opt/llvm/bin/clang-tidy" ]; then
        CLANG_TIDY="/usr/local/opt/llvm/bin/clang-tidy"
    else
        echo "clang-tidy not found. Please install llvm."
        exit 1
    fi
else
    CLANG_TIDY="clang-tidy"
fi

echo "Using clang-tidy: $CLANG_TIDY"

# Find source files (excluding external/build based on path) that are in the compile commands.
# run-clang-tidy is a common helper script, but we can call clang-tidy directly via find if unavailable.
# Or better, iterate over files directly.
# Let's use `find` similar to format_code.sh but pass -p to clang-tidy.

find . \
    \( -name "*.cpp" -o -name "*.c" -o -name "*.cc" -o -name "*.mm" -o -name "*.m" \) \
    -not -path "*/build/*" \
    -not -path "*/cmake-build-*" \
    -not -path "*/submodules/*" \
    -not -path "*/vcpkg_installed/*" \
    -not -path "*/.git/*" \
    -print0 | xargs -0 "$CLANG_TIDY" -p "$BUILD_DIR" --quiet

echo "Linting complete."
