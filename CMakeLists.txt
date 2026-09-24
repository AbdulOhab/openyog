# Phase 1 build (plan.md) — portable core + smoke test on Linux.
# Qt shell targets arrive in Phase 2 under qt/.
cmake_minimum_required(VERSION 3.21)
project(sqlyog-community-port LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Upstream's vcxproj builds Unicode (CreateFile etc. resolve to the ...W
# wide APIs); mingw defaults to ANSI, which breaks every wide-char Windows
# call in the ported code. Inert on native Linux (no windows.h there).
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)
endif()

# The bundled include/mysql (Connector/C 2.3.3) predates the system client lib;
# resolve <mysql/mysql.h> to the SYSTEM headers first. We do this by linking
# port/shim/mysql -> the dir containing mysql/mysql.h (same mechanism build.sh
# uses), so both build paths share one precedence story. The system
# include/mysql dir is ALSO added as an include path directly: upstream's
# Windows spellings (Datatype.h's flat <mysql.h> under _WIN32, live again in
# the mingw cross build) need mysql.h and its flat siblings (mariadb_com.h
# ...) lookup-able together, which a single symlinked header can't provide.
find_path(MDB_MYSQL_DIR mysql/mysql.h
          PATHS /usr/include /usr/local/include
          DOC "Directory containing mysql/mysql.h"
          REQUIRED)
# always refresh: a build tree configured for the other target (native Linux
# vs mingw-w64 cross) leaves a stale shim pointing at the wrong headers
file(REMOVE_RECURSE "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/mysql")
file(REMOVE "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/mysql.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/mysql")
file(CREATE_LINK "${MDB_MYSQL_DIR}/mysql" "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/mysql/mysql" SYMBOLIC)

# Same story for sqlite3.h: include/sqlite3.h is a bundled amalgamation
# header that predates the system libsqlite3 we link against below — shim
# it to the system header so headers and the linked .so actually match
# (also fixes src/wySqlite.cpp, previously only exercised via smoketest's
# own explicit ${SQLITE3_LIBRARY} link against the mismatched bundled header).
find_path(SYS_SQLITE3_DIR sqlite3.h
          PATHS /usr/include /usr/local/include
          DOC "Directory containing sqlite3.h"
          REQUIRED)
file(REMOVE "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/sqlite3.h")
file(CREATE_LINK "${SYS_SQLITE3_DIR}/sqlite3.h"
     "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/sqlite3.h" SYMBOLIC)

add_library(wycore STATIC
    src/wyString.cpp     # portable string class everything depends on
    src/wyFile.cpp       # POSIX port (PORT guards, Phase 1)
    src/wySqlite.cpp     # INI-adjacent helper store; upstream Linux bugs fixed
    src/wyIni.cpp        # INI engine = SQLyog's connection storage format
    port/stubs.cpp       # upstream-declared CRT externs + copied base64 helpers
)
target_compile_definitions(wycore PUBLIC COMMUNITY)   # upstream vcxproj default
target_include_directories(wycore PUBLIC port/shim include "${MDB_MYSQL_DIR}/mysql")
target_compile_options(wycore PRIVATE -Wall -Wno-unknown-pragmas)

find_package(Threads REQUIRED)
target_link_libraries(wycore PUBLIC Threads::Threads)

find_library(MARIADB_LIBRARY mariadb REQUIRED)
find_library(SQLITE3_LIBRARY sqlite3 REQUIRED)   # openyog's SqliteDriver needs it too now
find_library(PQ_LIBRARY pq REQUIRED)   # openyog's PostgresDriver — libpq-fe.h is
                                       # already on the default include path,
                                       # no header shim needed (unlike mysql/sqlite3)

add_executable(smoketest port/smoketest.cpp)
target_link_libraries(smoketest PRIVATE wycore ${MARIADB_LIBRARY} ${SQLITE3_LIBRARY})
target_compile_options(smoketest PRIVATE -Wall)

# ---- Phase 2: the Qt 6 application shell ("OpenYog", name approved 2026-09-08)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# The authentic SQLyog bitmaps (include/bitmaps) get compiled INTO the binary.
# qt/Icons.h used to read them from a path relative to the executable
# (<exe>/../include/bitmaps), which only ever resolved inside the build tree —
# a deployed Windows folder or an installed Linux prefix has no sibling source
# tree, so every icon silently came back as a null QIcon.
# Globbed, not listed: several Icons::get() call sites take the filename from
# runtime data (ObjectBrowser's node kinds, MainWindow's Create-object map), so
# a hand-written list would go stale without anything failing loudly.
set(BITMAP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include/bitmaps")
file(GLOB OPENYOG_BITMAPS CONFIGURE_DEPENDS RELATIVE "${BITMAP_DIR}"
     "${BITMAP_DIR}/*.ico" "${BITMAP_DIR}/*.ICO" "${BITMAP_DIR}/*.png")
set(BITMAP_QRC_BODY "")
foreach(bmp IN LISTS OPENYOG_BITMAPS)
    string(APPEND BITMAP_QRC_BODY "    <file alias=\"${bmp}\">${BITMAP_DIR}/${bmp}</file>\n")
endforeach()
# via a staging file + COPYONLY so an unchanged icon set doesn't re-run rcc
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bitmaps.qrc.in"
     "<!DOCTYPE RCC><RCC version=\"1.0\">\n<qresource prefix=\"/bitmaps\">\n${BITMAP_QRC_BODY}</qresource>\n</RCC>\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/bitmaps.qrc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/bitmaps.qrc" COPYONLY)

add_executable(openyog
    qt/main.cpp
    qt/openyog.qrc
    "${CMAKE_CURRENT_BINARY_DIR}/bitmaps.qrc"
    qt/db/IDbDriver.h
    qt/db/IDbConnection.h
    qt/db/DbDriverRegistry.h   qt/db/DbDriverRegistry.cpp
    qt/db/MySqlDriver.h        qt/db/MySqlDriver.cpp
    qt/db/MySqlConnection.h    qt/db/MySqlConnection.cpp
    qt/db/SqliteDriver.h       qt/db/SqliteDriver.cpp
    qt/db/SqliteConnection.h   qt/db/SqliteConnection.cpp
    qt/db/PostgresDriver.h     qt/db/PostgresDriver.cpp
    qt/db/PostgresConnection.h qt/db/PostgresConnection.cpp
    qt/MainWindow.h        qt/MainWindow.cpp
    qt/ConnectionDialog.h  qt/ConnectionDialog.cpp
    qt/ConnectionTab.h     qt/ConnectionTab.cpp
    qt/QueryModel.h        qt/QueryModel.cpp
    qt/ConnectionStore.h   qt/ConnectionStore.cpp
    qt/FavoritesStore.h    qt/FavoritesStore.cpp
    qt/ObjectBrowser.h     qt/ObjectBrowser.cpp
    qt/CreateTableDialog.h qt/CreateTableDialog.cpp
    qt/IndexDialog.h       qt/IndexDialog.cpp
    qt/SchemaSql.h         qt/SchemaSql.cpp
    qt/SqlSplit.h          qt/SqlSplit.cpp
    qt/ResultExport.h      qt/ResultExport.cpp
    qt/ExportDialog.h      qt/ExportDialog.cpp
    qt/UserManagerDialog.h qt/UserManagerDialog.cpp
    qt/ForeignKeyDialog.h  qt/ForeignKeyDialog.cpp
    qt/SqlDump.h           qt/SqlDump.cpp
    qt/SqlEditor.h         qt/SqlEditor.cpp
    qt/FindBar.h           qt/FindBar.cpp
    qt/SqlFormat.h         qt/SqlFormat.cpp
    qt/TableDataView.h     qt/TableDataView.cpp
    qt/FormView.h          qt/FormView.cpp
    qt/CustomFilterDialog.h qt/CustomFilterDialog.cpp
    qt/Theme.h             qt/Theme.cpp
)
# QScintilla (the query editor): system package, same pattern as the
# client libraries above — on Arch, `sudo pacman -S qscintilla-qt6`
# (headers land in /usr/include/qt6/Qsci, lib is libqscintilla2_qt6).
find_path(QSCINTILLA_INCLUDE_DIR Qsci/qsciscintilla.h
          HINTS /usr/include/qt6 /usr/local/include/qt6 REQUIRED)
find_library(QSCINTILLA_LIBRARY NAMES qscintilla2_qt6 REQUIRED)
target_include_directories(openyog PRIVATE ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(openyog PRIVATE wycore Qt6::Widgets ${MARIADB_LIBRARY} ${SQLITE3_LIBRARY} ${PQ_LIBRARY} ${QSCINTILLA_LIBRARY})
target_compile_options(openyog PRIVATE -Wall)

# Windows only: compile qt/openyog.rc so the .exe carries its own icon. windres
# resolves the script's "resources/openyog.ico" against its include path, not
# against the script's own directory, so qt/ has to be on it explicitly.
if(WIN32)
    enable_language(RC)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/qt")
    target_sources(openyog PRIVATE qt/openyog.rc)
endif()
