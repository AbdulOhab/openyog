# OpenYog ctest suite (enabled from the root CMakeLists.txt). Run with
#   ctest --test-dir <build-dir> --output-on-failure
find_package(Qt6 REQUIRED COMPONENTS Test)

# Unit tests compile the module under test straight from qt/: each one is a
# self-contained .cpp with only QtCore deps, so nothing has to be carved out
# of the openyog executable into a separate library first.
function(openyog_unit_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/qt")
    target_link_libraries(${name} PRIVATE Qt6::Test)
    target_compile_options(${name} PRIVATE -Wall)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

set(QT_SRC "${PROJECT_SOURCE_DIR}/qt")
openyog_unit_test(tst_sqlsplit     "${QT_SRC}/SqlSplit.cpp")
openyog_unit_test(tst_sqlformat    "${QT_SRC}/SqlFormat.cpp")
openyog_unit_test(tst_schemasql    "${QT_SRC}/SchemaSql.cpp" "${QT_SRC}/SqlSplit.cpp")
openyog_unit_test(tst_resultexport "${QT_SRC}/ResultExport.cpp")
openyog_unit_test(tst_delimitedtext "${QT_SRC}/DelimitedText.cpp" "${QT_SRC}/ResultExport.cpp")

# ConnectionStore against an in-memory keychain: FakeSecretStore.cpp takes
# qt/SecretStore.cpp's place, so the keychain, INI-fallback and migration
# paths all run without a real Secret Service
openyog_unit_test(tst_connectionstore FakeSecretStore.cpp "${QT_SRC}/ConnectionStore.cpp")
target_link_libraries(tst_connectionstore PRIVATE wycore ${MARIADB_LIBRARY} ${SQLITE3_LIBRARY})
# upstream headers (Datatype.h's MSVC pragmas, wyString.h's comments) — the
# same noise wycore itself silences
target_compile_options(tst_connectionstore PRIVATE -Wno-unknown-pragmas -Wno-comment)

# wycore port checks. Exit code 3 means the MariaDB server it also probes
# (127.0.0.1:3306) isn't reachable: a skip, not a failure, on a machine or
# CI runner without one.
add_test(NAME smoketest COMMAND smoketest)
set_tests_properties(smoketest PROPERTIES SKIP_RETURN_CODE 3)

# openyog's own headless selftests (see the flag list atop qt/main.cpp): the
# ones that need no database server. XDG_CONFIG_HOME keeps them away from
# the developer's real settings and saved connections.
set(SELFTEST_ENV "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg-config")
foreach(flag comptest stmtattest casetest filtertest icontest shortcuttest)
    add_test(NAME selftest_${flag} COMMAND openyog --${flag})
    set_tests_properties(selftest_${flag} PROPERTIES ENVIRONMENT "${SELFTEST_ENV}")
endforeach()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/exporttest")
add_test(NAME selftest_exporttest
         COMMAND openyog "--exporttest=${CMAKE_CURRENT_BINARY_DIR}/exporttest")
set_tests_properties(selftest_exporttest PROPERTIES ENVIRONMENT "${SELFTEST_ENV}")

# The SQLite selftests run against a database built from fixtures/ by the
# sqlite3 CLI, a fresh one per test and per run (all but --sqlitetest add
# tables of their own); without the CLI they're left out rather than failing.
find_program(SQLITE3_CLI sqlite3)
if(SQLITE3_CLI)
    foreach(flag sqlitetest sqlitecreatetabletest sqliteindextest sqlitemisctest)
        set(db "${CMAKE_CURRENT_BINARY_DIR}/fixture-${flag}.sqlite")
        add_test(NAME ${flag}_fixture
                 COMMAND "${CMAKE_COMMAND}" "-DSQLITE3=${SQLITE3_CLI}"
                         "-DSQL=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sqlite_fixture.sql"
                         "-DDB=${db}" -P "${CMAKE_CURRENT_SOURCE_DIR}/make_fixture_db.cmake")
        set_tests_properties(${flag}_fixture PROPERTIES FIXTURES_SETUP ${flag}_db)

        add_test(NAME selftest_${flag} COMMAND openyog "--${flag}=${db}")
        set_tests_properties(selftest_${flag} PROPERTIES
                             FIXTURES_REQUIRED ${flag}_db
                             ENVIRONMENT "${SELFTEST_ENV}")
    endforeach()
else()
    message(STATUS "sqlite3 CLI not found: skipping the SQLite selftests")
endif()
