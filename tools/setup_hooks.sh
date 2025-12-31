#!/bin/bash
set -e

# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

echo "Setting up pre-commit hooks..."

# Check for pre-commit
if ! command_exists pre-commit; then
    echo "pre-commit not found. Attempting to install..."
    if [[ "$OSTYPE" == "darwin"* ]]; then
        if command_exists brew; then
            brew install pre-commit
        else
            echo "Error: brew not found. Please install pre-commit manually."
            exit 1
        fi
    else
        if command_exists pip; then
            pip install pre-commit
        elif command_exists pip3; then
            pip3 install pre-commit
        else
             echo "Error: pip/pip3 not found. Please install pre-commit manually."
             exit 1
        fi
    fi
fi

# Check for clang-tidy
if ! command_exists clang-tidy; then
    echo "clang-tidy not found in PATH."
    if [[ -d "/opt/homebrew/opt/llvm/bin" ]]; then
        echo "Found llvm at /opt/homebrew/opt/llvm/bin. You may want to add this to your PATH:"
        echo 'export PATH="/opt/homebrew/opt/llvm/bin:$PATH"'
    elif [[ -d "/usr/local/opt/llvm/bin" ]]; then
        echo "Found llvm at /usr/local/opt/llvm/bin. You may want to add this to your PATH:"
        echo 'export PATH="/usr/local/opt/llvm/bin:$PATH"'
    else
        echo "Consider installing llvm via brew to get clang-tidy: brew install llvm"
    fi
fi

# Install the hooks
pre-commit install

echo "pre-commit hooks installed successfully!"
