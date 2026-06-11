#!/bin/bash
cd "$(dirname "$0")/.."

echo "Generating Xcode project for macOS..."
premake5 xcode4

if [ $? -eq 0 ]; then
    echo
    echo "Xcode project generated successfully!"
    echo
    echo "Projects created:"
    echo "  - Astra (Core Library - Header Only)"
    echo "  - AstraTest (Unit Tests with GoogleTest)"
    echo "  - AstraBenchmark (Performance Benchmarks)"
    echo "  - GoogleTest (Testing Framework)"
    echo
    echo "Open Astra.xcworkspace in Xcode to build."
    echo
    echo "Or build from command line:"
    echo "  xcodebuild -configuration Debug"
    echo "  xcodebuild -configuration Release"
    echo "  xcodebuild -configuration Dist"
else
    echo "Error: Failed to generate Xcode project"
    exit 1
fi