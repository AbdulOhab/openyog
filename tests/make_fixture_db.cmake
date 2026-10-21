# Builds a fresh SQLite fixture database for one selftest (tests/CMakeLists.txt):
#   cmake -DSQLITE3=<sqlite3 CLI> -DSQL=<fixture.sql> -DDB=<out.sqlite> -P make_fixture_db.cmake
# The old file is deleted first: the selftests leave tables of their own behind.
file(REMOVE "${DB}")
execute_process(COMMAND "${SQLITE3}" "${DB}" ".read ${SQL}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "sqlite3 exited with ${rc} building ${DB}")
endif()
