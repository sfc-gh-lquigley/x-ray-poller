#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "Compiling demo-backend-cpp..."
g++ -std=c++17 -O2 -Wall -Wextra \
    -o demo-backend-cpp main.cpp \
    -lcurl -lpthread

echo "Build complete: demo-backend-cpp"

echo ""
echo "Running propagator unit tests..."
g++ -std=c++17 -O2 -Wall -Wextra \
    -o xray_propagator_test xray_propagator_test.cpp
./xray_propagator_test
echo ""
echo "All tests passed. Ready to run: ./demo-backend-cpp"
