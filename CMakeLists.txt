# OpenYog: SQLyog's portable wy* core (wycore + smoketest) and the Qt 6
# application on top of it (openyog), plus its tests (tests/) and install
# rules (bottom of this file).
cmake_minimum_required(VERSION 3.21)
project(openyog VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# clangd needs the compile database (symlink compile_commands.json from the
# build dir to the repo root once; see .clangd for the flag-strip config)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Upstream's vcxproj builds Unicode (CreateFile etc. resolve to the ...W
# wide APIs); mingw defaults to ANSI, which breaks every wide-char Windows
# call in the ported code. Inert on native Linux (no windows.h there).
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)
endif()

# <mysql/mysql.h> and <sqlite3.h> resolve to the client libraries the build
# links: <build>/shim/mysql links to the system's mysql/ header dir and
# <build>/shim/sqlite3.h to its sqlite3.h, ahead of include/ on the include
# path (port/build.sh makes the same links in its own tree). The links live
# in the build tree, not the source tree: a native and a mingw-w64 cross
# build each point at their own headers. The system include/mysql dir is
# ALSO added as an include path directly: upstream's
# Windows spellings (Datatype.h's flat <mysql.h> under _WIN32, live again in
# the mingw cross build) need mysql.h and its flat siblings (mariadb_com.h
# ...) lookup-able together, which a single symlinked header can't provide.
find_path(MDB_MYSQL_DIR mysql/mysql.h
          PATHS /usr/include /usr/local/include
          DOC "Directory containing mysql/mysql.h"
          REQUIRED)
set(SHIM_DIR "${CMAKE_CURRENT_BINARY_DIR}/shim")
file(REMOVE_RECURSE "${SHIM_DIR}")
file(MAKE_DIRECTORY "${SHIM_DIR}")
file(CREATE_LINK "${MDB_MYSQL_DIR}/mysql" "${SHIM_DIR}/mysql" SYMBOLIC)
# where older configures put these links: left there, they'd shadow the
# build tree's (port/shim is still on the include path for its own headers)
file(REMOVE_RECURSE "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/mysql")
file(REMOVE "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/mysql.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/port/shim/sqlite3.h")

# Same for sqlite3.h, the system libsqlite3's header.
find_path(SYS_SQLITE3_DIR sqlite3.h
          PATHS /usr/include /usr/local/include
          DOC "Directory containing sqlite3.h"
          REQUIRED)
file(CREATE_LINK "${SYS_SQLITE3_DIR}/sqlite3.h" "${SHIM_DIR}/sqlite3.h" SYMBOLIC)

add_library(wycore STATIC
    src/wyString.cpp     # portable string class everything depends on
    src/wyFile.cpp       # POSIX port (PORT guards, Phase 1)
    src/wySqlite.cpp     # INI-adjacent helper store; upstream Linux bugs fixed
    src/wyIni.cpp        # INI engine = SQLyog's connection storage format
    port/stubs.cpp       # upstream-declared CRT externs + copied base64 helpers
)
target_compile_definitions(wycore PUBLIC COMMUNITY)   # upstream vcxproj default
target_include_directories(wycore PUBLIC "${SHIM_DIR}" port/shim include "${MDB_MYSQL_DIR}/mysql")
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
# zlib: reading .xlsx workbooks (zip files) in Import External Data
find_package(ZLIB REQUIRED)
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
    qt/Selftest.h qt/SelftestSqlite.cpp qt/SelftestServer.cpp qt/SelftestMisc.cpp qt/SelftestMongo.cpp # standalone --selftest flags
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
    qt/db/SshTunnel.h          qt/db/SshTunnel.cpp
    qt/MainWindow.h        qt/MainWindow.cpp
    qt/MainWindowMenus.cpp qt/MainWindowToolsMenus.cpp qt/MainWindowSelftest.cpp # MainWindow, split by area
    qt/ConnectionDialog.h  qt/ConnectionDialog.cpp
    qt/ConnectionTab.h     qt/ConnectionTab.cpp
    qt/ConnectionTabCopy.cpp   qt/ConnectionTabImport.cpp   # ConnectionTab, split by area
    qt/ConnectionTabSchema.cpp qt/ConnectionTabTools.cpp
    qt/QueryModel.h        qt/QueryModel.cpp
    qt/ConnectionStore.h   qt/ConnectionStore.cpp
    qt/SessionStore.h      qt/SessionStore.cpp
    qt/SecretStore.h       qt/SecretStore.cpp
    qt/FavoritesStore.h    qt/FavoritesStore.cpp
    qt/ObjectBrowser.h     qt/ObjectBrowser.cpp
    qt/CreateTableDialog.h qt/CreateTableDialog.cpp
    qt/IndexDialog.h       qt/IndexDialog.cpp
    qt/SchemaSql.h         qt/SchemaSql.cpp
    qt/SqlSplit.h          qt/SqlSplit.cpp
    qt/SqlCompletion.h     qt/SqlCompletion.cpp
    qt/ResultExport.h      qt/ResultExport.cpp
    qt/DelimitedText.h     qt/DelimitedText.cpp
    qt/ExportDialog.h      qt/ExportDialog.cpp
    qt/UserManagerDialog.h qt/UserManagerDialog.cpp
    qt/ForeignKeyDialog.h  qt/ForeignKeyDialog.cpp
    qt/SqlDump.h           qt/SqlDump.cpp
    qt/SqlDumpDialog.h     qt/SqlDumpDialog.cpp
    qt/SqlEditor.h         qt/SqlEditor.cpp
    qt/FindBar.h           qt/FindBar.cpp
    qt/SqlFormat.h         qt/SqlFormat.cpp
    qt/TableDataView.h     qt/TableDataView.cpp
    qt/TableDataModel.h
    qt/TableDataViewEdit.cpp qt/TableDataViewMenu.cpp qt/TableDataViewForm.cpp # TableDataView, split by area
    qt/FormView.h          qt/FormView.cpp
    qt/InfoPanel.h         qt/InfoPanel.cpp
    qt/CustomFilterDialog.h qt/CustomFilterDialog.cpp
    qt/Theme.h             qt/Theme.cpp
    qt/DiagramItems.h      qt/DiagramItems.cpp
    qt/SchemaDiagram.h     qt/SchemaDiagram.cpp
    qt/SchemaDesigner.h    qt/SchemaDesigner.cpp
    qt/QueryBuild.h        qt/QueryBuild.cpp
    qt/QueryBuilder.h      qt/QueryBuilder.cpp
    qt/SchemaSync.h        qt/SchemaSync.cpp
    qt/PrivateFiles.h      qt/PrivateFiles.cpp
    qt/SchemaSyncDialog.h  qt/SchemaSyncDialog.cpp
    qt/DataSync.h          qt/DataSync.cpp
    qt/DataSyncDialog.h    qt/DataSyncDialog.cpp
    qt/MongoJson.h         qt/MongoJson.cpp
    qt/db/MongoClient.h    qt/db/MongoClient.cpp
    qt/MongoTab.h          qt/MongoTab.cpp
    qt/ExternalData.h      qt/ExternalData.cpp
    qt/ExternalDataDialog.h qt/ExternalDataDialog.cpp
)
# QScintilla (the query editor): system package, same pattern as the
# client libraries above — on Arch, `sudo pacman -S qscintilla-qt6`
# (headers land in /usr/include/qt6/Qsci, lib is libqscintilla2_qt6).
find_path(QSCINTILLA_INCLUDE_DIR Qsci/qsciscintilla.h
          HINTS /usr/include/qt6 /usr/local/include/qt6 PATH_SUFFIXES qt6 REQUIRED)
find_library(QSCINTILLA_LIBRARY NAMES qscintilla2_qt6 REQUIRED)
target_include_directories(openyog PRIVATE ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(openyog PRIVATE wycore Qt6::Widgets ZLIB::ZLIB ${MARIADB_LIBRARY} ${SQLITE3_LIBRARY} ${PQ_LIBRARY} ${QSCINTILLA_LIBRARY})
target_compile_options(openyog PRIVATE -Wall)
target_compile_definitions(openyog PRIVATE OPENYOG_VERSION="${PROJECT_VERSION}")

# Saved-connection passwords go to the OS keychain through QtKeychain (Arch:
# `sudo pacman -S qtkeychain-qt6`; the Windows build's comes with the MSYS2
# sysroot). Optional: without it SecretStore compiles to stubs and
# ConnectionStore keeps upstream's base64 field in connections.ini.
find_package(Qt6Keychain CONFIG QUIET)
if(Qt6Keychain_FOUND)
    target_compile_definitions(openyog PRIVATE OPENYOG_HAVE_KEYCHAIN)
    target_link_libraries(openyog PRIVATE Qt6Keychain::Qt6Keychain)
    message(STATUS "QtKeychain found: saved passwords go to the OS keychain")
else()
    message(STATUS "QtKeychain not found: saved passwords stay base64 in connections.ini")
endif()

# MongoDB connections (qt/db/MongoClient, the Mongo tab) through libmongoc 2
# (Arch: mongo-c-driver; the Windows build: MSYS2's). Optional: without it
# MongoClient compiles to stubs and the connection dialog says it isn't
# available.
find_package(mongoc 2.0 CONFIG QUIET)
if(mongoc_FOUND)
    target_compile_definitions(openyog PRIVATE OPENYOG_HAVE_MONGO)
    if(WIN32)
        # MSYS2's package config names Windows libraries as on a
        # case-insensitive disk (-lBcrypt) and some of its own by paths on
        # the machine that built it; using the DLLs needs only their two
        # import libraries
        find_library(MONGOC_IMPORT_LIBRARY NAMES mongoc2 REQUIRED)
        find_library(BSON_IMPORT_LIBRARY NAMES bson2 REQUIRED)
        get_target_property(MONGOC_INCLUDE_DIRS mongoc::shared INTERFACE_INCLUDE_DIRECTORIES)
        target_include_directories(openyog PRIVATE ${MONGOC_INCLUDE_DIRS})
        target_link_libraries(openyog PRIVATE ${MONGOC_IMPORT_LIBRARY} ${BSON_IMPORT_LIBRARY})
    else()
        target_link_libraries(openyog PRIVATE mongoc::shared)
    endif()
    message(STATUS "libmongoc ${mongoc_VERSION} found: MongoDB enabled")
else()
    message(STATUS "libmongoc 2 not found: MongoDB disabled")
endif()

# SSH tunnels (qt/db/SshTunnel) through libssh (Arch: libssh; the Windows
# build: MSYS2's). Optional, on Linux and Windows (the forwarding loop is
# POSIX sockets or Winsock): without it SshTunnel compiles to stubs and the
# SSH tab says it isn't available.
find_path(LIBSSH_INCLUDE_DIR libssh/libssh.h)
find_library(LIBSSH_LIBRARY NAMES ssh)
if(LIBSSH_INCLUDE_DIR AND LIBSSH_LIBRARY AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32))
    target_compile_definitions(openyog PRIVATE OPENYOG_HAVE_SSH)
    target_include_directories(openyog PRIVATE ${LIBSSH_INCLUDE_DIR})
    target_link_libraries(openyog PRIVATE ${LIBSSH_LIBRARY})
    if(WIN32)
        target_link_libraries(openyog PRIVATE ws2_32)
    endif()
    message(STATUS "libssh found: SSH tunnels enabled")
else()
    message(STATUS "libssh not found (or not Linux / Windows): SSH tunnels disabled")
endif()

# Windows only: compile qt/openyog.rc so the .exe carries its own icon. windres
# resolves the script's "resources/openyog.ico" against its include path, not
# against the script's own directory, so qt/ has to be on it explicitly.
if(WIN32)
    enable_language(RC)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/qt")
    target_sources(openyog PRIVATE qt/openyog.rc)
endif()

# ---- Tests: unit tests for qt/'s pure-logic modules plus smoketest and the
# openyog selftests, all under ctest (tests/CMakeLists.txt). On by default;
# -DOPENYOG_BUILD_TESTS=OFF skips the extra targets (e.g. the mingw cross
# build, whose executables can't run on the build host anyway).
option(OPENYOG_BUILD_TESTS "Build the ctest suite under tests/" ON)
if(OPENYOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Install: the binary, plus on Linux the desktop entry, AppStream
# metadata and icons under the reverse-DNS app id that QGuiApplication::
# setDesktopFileName() (qt/main.cpp) announces to the compositor.
#   cmake --install <build> [--prefix /usr] [DESTDIR=… for packaging]
include(GNUInstallDirs)
install(TARGETS openyog RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    set(OPENYOG_APP_ID io.github.abdulohab.openyog)
    install(FILES packaging/linux/${OPENYOG_APP_ID}.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/linux/${OPENYOG_APP_ID}.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    foreach(size 16 24 32 64 256)
        install(FILES qt/resources/openyog-${size}.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
                RENAME ${OPENYOG_APP_ID}.png)
    endforeach()
    install(FILES qt/openyog.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
            RENAME ${OPENYOG_APP_ID}.svg)
    install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
