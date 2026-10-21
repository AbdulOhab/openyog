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
openyog_unit_test(tst_sqlcompletion "${QT_SRC}/SqlCompletion.cpp" "${QT_SRC}/SqlSplit.cpp")
openyog_unit_test(tst_sessionstore "${QT_SRC}/SessionStore.cpp")

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
# the developer's real settings and saved connections, a dead session bus
# (plus a private XDG_RUNTIME_DIR, so GLib can't fall back to the real one)
# keeps a connection they save out of the developer's keyring, and
# OPENYOG_SSH_DIR / no ssh-agent keep the SSH tests off ~/.ssh.
set(SELFTEST_RUNTIME "${CMAKE_CURRENT_BINARY_DIR}/xdg-runtime")
file(MAKE_DIRECTORY "${SELFTEST_RUNTIME}")
file(CHMOD "${SELFTEST_RUNTIME}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(SELFTEST_ENV
    "QT_QPA_PLATFORM=offscreen"
    "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg-config"
    "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg-data"
    "XDG_RUNTIME_DIR=${SELFTEST_RUNTIME}"
    "OPENYOG_SSH_DIR=${CMAKE_CURRENT_BINARY_DIR}/ssh"
    "SSH_AUTH_SOCK="
    "DBUS_SESSION_BUS_ADDRESS=unix:path=${SELFTEST_RUNTIME}/no-bus")
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
    foreach(flag sqlitetest sqlitecreatetabletest sqliteindextest sqlitemisctest sessiontest)
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

    # SQL dump → restore keeps binary and non-ASCII data byte for byte
    set(db "${CMAKE_CURRENT_BINARY_DIR}/fixture-dump.sqlite")
    add_test(NAME dump_fixture
             COMMAND "${CMAKE_COMMAND}" "-DSQLITE3=${SQLITE3_CLI}"
                     "-DSQL=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sqlite_fixture.sql"
                     "-DDB=${db}" -P "${CMAKE_CURRENT_SOURCE_DIR}/make_fixture_db.cmake")
    set_tests_properties(dump_fixture PROPERTIES FIXTURES_SETUP dump_db)
    add_test(NAME selftest_dump_roundtrip
             COMMAND "${CMAKE_COMMAND}" "-DOPENYOG=$<TARGET_FILE:openyog>"
                     "-DSQLITE3=${SQLITE3_CLI}" "-DSRC=${db}"
                     "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/dump-roundtrip"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/dump_roundtrip.cmake")
    set_tests_properties(selftest_dump_roundtrip PROPERTIES
                         FIXTURES_REQUIRED dump_db
                         ENVIRONMENT "${SELFTEST_ENV}")
else()
    message(STATUS "sqlite3 CLI not found: skipping the SQLite selftests")
endif()

# ---- Live database servers (optional) ---------------------------------------
# -DOPENYOG_TEST_MYSQL=host:port:user:password:db and/or
# -DOPENYOG_TEST_PG=host:port:user:password:db add the selftests that need a
# real server; CI runs them against throwaway service containers. Their
# fixtures DROP and recreate their own tables (PostgreSQL also creates
# <db>_other, and the dump round trips a scratch <db>_restore): never point
# these at a database that matters. The password can't contain ':'.
set(OPENYOG_TEST_MYSQL "" CACHE STRING
    "host:port:user:password:db of a throwaway MySQL/MariaDB database for live selftests")
set(OPENYOG_TEST_PG "" CACHE STRING
    "host:port:user:password:db of a throwaway PostgreSQL database for live selftests")
set(LIVE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/live_server.cmake")

if(OPENYOG_TEST_MYSQL)
    find_program(MYSQL_CLI NAMES mariadb mysql REQUIRED)
    string(REPLACE ":" ";" my "${OPENYOG_TEST_MYSQL}")
    list(GET my 0 my_host)
    list(GET my 1 my_port)
    list(GET my 2 my_user)
    list(GET my 3 my_password)
    list(GET my 4 my_db)

    add_test(NAME mysql_fixture
             COMMAND "${CMAKE_COMMAND}" -DMODE=mysql-fixture "-DSERVER=${OPENYOG_TEST_MYSQL}"
                     "-DCLI=${MYSQL_CLI}" "-DSQL=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mysql_fixture.sql"
                     -P "${LIVE_SCRIPT}")
    set_tests_properties(mysql_fixture PROPERTIES FIXTURES_SETUP mysql_db RESOURCE_LOCK mysql_db)

    add_test(NAME smoketest_mysql COMMAND smoketest ${my_host} ${my_user} ${my_password} ${my_port} ${my_db})
    add_test(NAME selftest_mysql_schema COMMAND openyog "--schematest=${OPENYOG_TEST_MYSQL}")
    add_test(NAME selftest_mysql_dump_roundtrip
             COMMAND "${CMAKE_COMMAND}" -DMODE=mysql-roundtrip "-DSERVER=${OPENYOG_TEST_MYSQL}"
                     "-DCLI=${MYSQL_CLI}" "-DOPENYOG=$<TARGET_FILE:openyog>"
                     "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/mysql-roundtrip" -P "${LIVE_SCRIPT}")
    # autocomplete against the real schema: an alias's columns, a procedure
    add_test(NAME selftest_mysql_complete
             COMMAND openyog "--autoconnect=${OPENYOG_TEST_MYSQL}"
                     "--screenshot=${CMAKE_CURRENT_BINARY_DIR}/mysql-complete.png"
                     "--completeat=SELECT * FROM employees e WHERE e." "--completeat=CALL ")
    set_tests_properties(selftest_mysql_complete PROPERTIES PASS_REGULAR_EXPRESSION
        "e\\.\\]: id \\| name \\| salary \\| city \\| dept_id \\| hired \\| photo\ncompleteat \\[CALL \\]: refresh_stats\\(")
    # a JOIN's ON: the condition its foreign key spells out
    add_test(NAME selftest_mysql_complete_join
             COMMAND openyog "--autoconnect=${OPENYOG_TEST_MYSQL}"
                     "--screenshot=${CMAKE_CURRENT_BINARY_DIR}/mysql-complete-join.png"
                     "--completeat=SELECT * FROM employees e JOIN emp_dept d ON ")
    set_tests_properties(selftest_mysql_complete_join PROPERTIES PASS_REGULAR_EXPRESSION
        "ON \\]: d\\.id = e\\.dept_id \\|")
    set_tests_properties(smoketest_mysql selftest_mysql_schema selftest_mysql_dump_roundtrip
                         selftest_mysql_complete selftest_mysql_complete_join PROPERTIES
                         FIXTURES_REQUIRED mysql_db RESOURCE_LOCK mysql_db
                         ENVIRONMENT "${SELFTEST_ENV}")
endif()

if(OPENYOG_TEST_PG)
    find_program(PSQL_CLI NAMES psql REQUIRED)
    string(REPLACE ":" ";" pg "${OPENYOG_TEST_PG}")
    list(GET pg 4 pg_db)

    add_test(NAME pg_fixture
             COMMAND "${CMAKE_COMMAND}" -DMODE=pg-fixture "-DSERVER=${OPENYOG_TEST_PG}"
                     "-DCLI=${PSQL_CLI}" "-DSQL=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pg_fixture.sql"
                     "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/pg-fixture" -P "${LIVE_SCRIPT}")
    set_tests_properties(pg_fixture PROPERTIES FIXTURES_SETUP pg_db RESOURCE_LOCK pg_db)

    add_test(NAME selftest_pg_metadata COMMAND openyog "--pgtest=${OPENYOG_TEST_PG}")
    add_test(NAME selftest_pg_schema COMMAND openyog "--pgschematest=${OPENYOG_TEST_PG}")
    add_test(NAME selftest_pg_multidb
             COMMAND openyog "--autoconnectpg=${OPENYOG_TEST_PG}" "--pgmultidbtest=${pg_db}_other")
    add_test(NAME selftest_pg_dump_roundtrip
             COMMAND "${CMAKE_COMMAND}" -DMODE=pg-roundtrip "-DSERVER=${OPENYOG_TEST_PG}"
                     "-DCLI=${PSQL_CLI}" "-DOPENYOG=$<TARGET_FILE:openyog>"
                     "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/pg-roundtrip" -P "${LIVE_SCRIPT}")
    add_test(NAME selftest_pg_complete
             COMMAND openyog "--autoconnectpg=${OPENYOG_TEST_PG}"
                     "--screenshot=${CMAKE_CURRENT_BINARY_DIR}/pg-complete.png"
                     "--completeat=SELECT * FROM v_emp" "--completeat=SELECT * FROM employees WHERE name ili")
    set_tests_properties(selftest_pg_complete PROPERTIES PASS_REGULAR_EXPRESSION
        "v_emp\\]: v_employee_dept\ncompleteat \\[[^]]*ili\\]: ILIKE")
    add_test(NAME selftest_pg_complete_join
             COMMAND openyog "--autoconnectpg=${OPENYOG_TEST_PG}"
                     "--screenshot=${CMAKE_CURRENT_BINARY_DIR}/pg-complete-join.png"
                     "--completeat=SELECT * FROM employees e JOIN emp_dept d ON ")
    set_tests_properties(selftest_pg_complete_join PROPERTIES PASS_REGULAR_EXPRESSION
        "ON \\]: d\\.id = e\\.dept_id \\|")
    set_tests_properties(selftest_pg_metadata selftest_pg_schema selftest_pg_multidb
                         selftest_pg_dump_roundtrip selftest_pg_complete selftest_pg_complete_join
                         PROPERTIES
                         FIXTURES_REQUIRED pg_db RESOURCE_LOCK pg_db
                         ENVIRONMENT "${SELFTEST_ENV}")
endif()

# ---- SSH tunnel (optional, on top of the live servers above) ----------------
# -DOPENYOG_TEST_SSH=host:port:user:password of an SSH server that allows
# TCP forwarding; the MySQL/PostgreSQL selftests then also run through it.
# The database as seen from that SSH host defaults to the targets above;
# -DOPENYOG_TEST_SSH_MYSQL / -DOPENYOG_TEST_SSH_PG override it (e.g. an sshd
# in a container reaching the host's servers as host.containers.internal).
set(OPENYOG_TEST_SSH "" CACHE STRING
    "host:port:user:password of an SSH server for the tunnel selftests")
set(OPENYOG_TEST_SSH_MYSQL "${OPENYOG_TEST_MYSQL}" CACHE STRING
    "the MySQL test database as seen from the SSH host")
set(OPENYOG_TEST_SSH_PG "${OPENYOG_TEST_PG}" CACHE STRING
    "the PostgreSQL test database as seen from the SSH host")
if(OPENYOG_TEST_SSH AND OPENYOG_TEST_MYSQL)
    add_test(NAME selftest_mysql_ssh
             COMMAND openyog "--autoconnect=${OPENYOG_TEST_SSH_MYSQL}" "--ssh=${OPENYOG_TEST_SSH}"
                     --sshtrust "--screenshot=${CMAKE_CURRENT_BINARY_DIR}/mysql-ssh.png"
                     "--completeat=SELECT * FROM employees e WHERE e.")
    set_tests_properties(selftest_mysql_ssh PROPERTIES
                         PASS_REGULAR_EXPRESSION "e\\.\\]: id \\| name \\| salary"
                         FIXTURES_REQUIRED mysql_db RESOURCE_LOCK mysql_db
                         ENVIRONMENT "${SELFTEST_ENV}")
endif()
if(OPENYOG_TEST_SSH AND OPENYOG_TEST_PG)
    add_test(NAME selftest_pg_ssh
             COMMAND openyog "--autoconnectpg=${OPENYOG_TEST_SSH_PG}" "--ssh=${OPENYOG_TEST_SSH}"
                     --sshtrust "--screenshot=${CMAKE_CURRENT_BINARY_DIR}/pg-ssh.png"
                     "--completeat=SELECT * FROM employees e JOIN emp_dept d ON ")
    set_tests_properties(selftest_pg_ssh PROPERTIES
                         PASS_REGULAR_EXPRESSION "ON \\]: d\\.id = e\\.dept_id \\|"
                         FIXTURES_REQUIRED pg_db RESOURCE_LOCK pg_db
                         ENVIRONMENT "${SELFTEST_ENV}")
endif()
