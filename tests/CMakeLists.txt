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

# openyog's own headless selftests (see the flag list atop qt/main.cpp)
add_test(NAME selftest_comptest COMMAND openyog --comptest)
set_tests_properties(selftest_comptest PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# --sqlitetest runs against a database built from fixtures/ by the sqlite3
# CLI; without the CLI both tests are left out rather than failing.
find_program(SQLITE3_CLI sqlite3)
if(SQLITE3_CLI)
    set(SQLITE_FIXTURE_DB "${CMAKE_CURRENT_BINARY_DIR}/sqlite_fixture.sqlite")
    add_test(NAME sqlite_fixture_db
             COMMAND "${SQLITE3_CLI}" "${SQLITE_FIXTURE_DB}"
                     ".read ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sqlite_fixture.sql")
    set_tests_properties(sqlite_fixture_db PROPERTIES FIXTURES_SETUP sqlite_fixture)

    add_test(NAME selftest_sqlite COMMAND openyog "--sqlitetest=${SQLITE_FIXTURE_DB}")
    set_tests_properties(selftest_sqlite PROPERTIES
                         FIXTURES_REQUIRED sqlite_fixture
                         ENVIRONMENT QT_QPA_PLATFORM=offscreen)
else()
    message(STATUS "sqlite3 CLI not found: skipping the --sqlitetest selftest")
endif()
