#!/bin/bash

# Find and format all C/C++/ObjC files
# Excludes build directories and submodules/vcpkg artifacts via -prune if commonly named,
# but relying on hidden dirs mainly.
# Better leverage git ls-files if inside a repo, or find for general usage.
# Using find to be robust if git is not initialized or specific subdirs are desired.

DIRS="."
if [ "$#" -gt 0 ]; then
    DIRS="$@"
fi

echo "Formatting code in: $DIRS"

# Check for clang-format
if ! command -v clang-format &> /dev/null; then
    echo "clang-format not found. Please install it (e.g., brew install llvm)."
    exit 1
fi

find $DIRS \
    \( -name "*.cpp" -o -name "*.h" -o -name "*.c" -o -name "*.hpp" -o -name "*.cc" -o -name "*.mm" -o -name "*.m" \) \
    -not -path "*/build/*" \
    -not -path "*/cmake-build-*" \
    -not -path "*/submodules/*" \
    -not -path "*/vcpkg_installed/*" \
    -not -path "*/.git/*" \
    -print0 | xargs -0 clang-format -i -style=file

echo "Formatting complete."
