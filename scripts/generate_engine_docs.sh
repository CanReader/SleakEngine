#!/usr/bin/env bash
set -e

# Script to generate Doxygen HTML documentation for SleakEngine

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ENGINE_DIR}"

if ! command -v doxygen &> /dev/null; then
    echo "Error: Doxygen is not installed or not in PATH."
    exit 1
fi

echo "==============================================="
echo "Generating Doxygen Documentation for SleakEngine"
echo "==============================================="

doxygen Doxyfile

echo ""
echo "==============================================="
echo "SleakEngine Documentation successfully generated!"
echo "Main Page: Engine/docs/html/index.html"
echo "==============================================="
