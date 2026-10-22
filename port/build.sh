#!/usr/bin/env bash
# port/build.sh — Phase 1 bootstrap build (plain g++).
# Temporary until cmake is installed; now that cmake exists, prefer:
#   cmake -S . -B build-cmake -G Ninja && cmake --build build-cmake
# Kept as a fast single-shot builder for iteration.
set -euo pipefail
cd "$(dirname "$0")/.."

# the system mysql/sqlite3 headers ahead of the bundled ones in include/,
# the same links CMakeLists.txt makes in its build tree
mkdir -p build-core/shim
ln -sfn /usr/include/mysql build-core/shim/mysql
ln -sf /usr/include/sqlite3.h build-core/shim/sqlite3.h
CXXFLAGS="-std=c++17 -Wall -Wno-unknown-pragmas -DCOMMUNITY -Ibuild-core/shim -Iport/shim -Iinclude"

for src in wyString wyFile wySqlite wyIni; do
    g++ $CXXFLAGS -c src/$src.cpp -o build-core/$src.o
done
g++ $CXXFLAGS -c port/stubs.cpp     -o build-core/stubs.o
g++ $CXXFLAGS -c port/smoketest.cpp -o build-core/smoketest.o
g++ build-core/wyString.o build-core/wyFile.o build-core/wySqlite.o \
    build-core/wyIni.o build-core/stubs.o build-core/smoketest.o \
    -lmariadb -lsqlite3 -lpthread -o build-core/smoketest

echo "OK: build-core/smoketest"
