#!/bin/bash
cd "$(dirname "$0")/.."

echo "Generating Makefiles for Linux..."
premake5 gmake2

if [ $? -eq 0 ]; then
    echo
    echo "Makefiles generated successfully!"
    echo
    echo "Projects created:"
    echo "  - Astra (Core Library - Header Only)"
    echo "  - AstraTest (Unit Tests with GoogleTest)"
    echo "  - AstraBenchmark (Performance Benchmarks)"
    echo "  - GoogleTest (Testing Framework)"
    echo
    echo "To build:"
    echo "  make config=debug       # Debug build"
    echo "  make config=release     # Release build"
    echo "  make config=dist        # Distribution build"
    echo
    echo "To build specific project:"
    echo "  make config=release AstraBenchmark"
    echo "  make config=debug AstraTest"
else
    echo "Error: Failed to generate makefiles"
    exit 1
fi