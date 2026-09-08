# Phase 1 build (plan.md) — portable core + smoke test on Linux.
# Qt shell targets arrive in Phase 2 under qt/.
cmake_minimum_required(VERSION 3.21)
project(sqlyog-community-port LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The bundled include/mysql (Connector/C 2.3.3) predates the system client lib;
# resolve <mysql/mysql.h> to the SYSTEM headers first. We do this by linking
# port/shim/mysql -> the dir containing mysql/mysql.h (same mechanism build.sh
# uses), so both build paths share one precedence story.
find_path(MDB_MYSQL_DIR mysql/mysql.h
          PATHS /usr/include /usr/local/include
          DOC "Directory containing mysql/mysql.h"
          REQUIRED)
get_filename_component(MDB_PARENT "${MDB_MYSQL_DIR}" DIRECTORY)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/mysql")
    file(CREATE_LINK "${MDB_PARENT}" "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/mysql" SYMBOLIC)
endif()

add_library(wycore STATIC
    src/wyString.cpp     # portable string class everything depends on
    port/stubs.cpp       # upstream-declared CRT externs + copied base64 helpers
)
target_include_directories(wycore PUBLIC port/shim include)
target_compile_options(wycore PRIVATE -Wall -Wno-unknown-pragmas)

find_library(MARIADB_LIBRARY mariadb REQUIRED)

add_executable(smoketest port/smoketest.cpp)
target_link_libraries(smoketest PRIVATE wycore ${MARIADB_LIBRARY})
target_compile_options(smoketest PRIVATE -Wall)
