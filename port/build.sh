#!/usr/bin/env bash
# port/build.sh — Phase 1 bootstrap build (plain g++).
# Temporary until cmake is installed; then CMakeLists.txt at repo root takes
# over and this script shrinks to a convenience wrapper (or dies).
set -euo pipefail
cd "$(dirname "$0")/.."

CXXFLAGS="-std=c++17 -Wall -Wno-unknown-pragmas -Iport/shim -Iinclude"

mkdir -p build-core
g++ $CXXFLAGS -c src/wyString.cpp    -o build-core/wyString.o
g++ $CXXFLAGS -c port/stubs.cpp      -o build-core/stubs.o
g++ $CXXFLAGS -c port/smoketest.cpp  -o build-core/smoketest.o
g++ build-core/wyString.o build-core/stubs.o build-core/smoketest.o \
    -lmariadb -o build-core/smoketest

echo "OK: build-core/smoketest"
