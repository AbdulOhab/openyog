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
    src/wyFile.cpp       # POSIX port (PORT guards, Phase 1)
    src/wySqlite.cpp     # INI-adjacent helper store; upstream Linux bugs fixed
    src/wyIni.cpp        # INI engine = SQLyog's connection storage format
    port/stubs.cpp       # upstream-declared CRT externs + copied base64 helpers
)
target_compile_definitions(wycore PUBLIC COMMUNITY)   # upstream vcxproj default
target_include_directories(wycore PUBLIC port/shim include)
target_compile_options(wycore PRIVATE -Wall -Wno-unknown-pragmas)

find_package(Threads REQUIRED)
target_link_libraries(wycore PUBLIC Threads::Threads)

find_library(MARIADB_LIBRARY mariadb REQUIRED)
find_library(SQLITE3_LIBRARY sqlite3)

add_executable(smoketest port/smoketest.cpp)
target_link_libraries(smoketest PRIVATE wycore ${MARIADB_LIBRARY})
if(SQLITE3_LIBRARY)
    target_link_libraries(smoketest PRIVATE ${SQLITE3_LIBRARY})
endif()
target_compile_options(smoketest PRIVATE -Wall)

# ---- Phase 2: the Qt 6 application shell ("OpenYog", name approved 2026-09-08)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)

add_executable(openyog
    qt/main.cpp
    qt/MainWindow.h        qt/MainWindow.cpp
    qt/ConnectionDialog.h  qt/ConnectionDialog.cpp
    qt/ConnectionTab.h     qt/ConnectionTab.cpp
    qt/QueryModel.h        qt/QueryModel.cpp
    qt/ConnectionStore.h   qt/ConnectionStore.cpp
    qt/ObjectBrowser.h     qt/ObjectBrowser.cpp
    qt/CodeEditor.h        qt/CodeEditor.cpp
    qt/Theme.h             qt/Theme.cpp
    qt/SqlHighlighter.h
)
target_link_libraries(openyog PRIVATE wycore Qt6::Widgets ${MARIADB_LIBRARY})
target_compile_options(openyog PRIVATE -Wall)
