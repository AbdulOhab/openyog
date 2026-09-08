#!/usr/bin/env bash
# port/build.sh — Phase 1 bootstrap build (plain g++).
# Temporary until cmake is installed; now that cmake exists, prefer:
#   cmake -S . -B build-cmake -G Ninja && cmake --build build-cmake
# Kept as a fast single-shot builder for iteration.
set -euo pipefail
cd "$(dirname "$0")/.."

CXXFLAGS="-std=c++17 -Wall -Wno-unknown-pragmas -DCOMMUNITY -Iport/shim -Iinclude"

mkdir -p build-core
for src in wyString wyFile wySqlite wyIni; do
    g++ $CXXFLAGS -c src/$src.cpp -o build-core/$src.o
done
g++ $CXXFLAGS -c port/stubs.cpp     -o build-core/stubs.o
g++ $CXXFLAGS -c port/smoketest.cpp -o build-core/smoketest.o
g++ build-core/wyString.o build-core/wyFile.o build-core/wySqlite.o \
    build-core/wyIni.o build-core/stubs.o build-core/smoketest.o \
    -lmariadb -lsqlite3 -lpthread -o build-core/smoketest

echo "OK: build-core/smoketest"
