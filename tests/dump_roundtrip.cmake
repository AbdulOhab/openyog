# SQL dump → restore round trip on a SQLite database (tests/CMakeLists.txt):
#   cmake -DOPENYOG=<exe> -DSQLITE3=<cli> -DSRC=<db> -DOUT=<dir> -P dump_roundtrip.cmake
# Dumps SRC with `openyog --dumpdb`, restores the dump into a fresh database
# with the sqlite3 CLI, and compares a BLOB and a non-ASCII text column byte
# for byte — a dump that silently mangles binary data restores "fine".
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${OPENYOG}" "--autoconnectfile=${SRC}" "--dumpdb=${OUT}/dump.sql"
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "openyog --dumpdb exited with ${rc}")
endif()
execute_process(COMMAND "${SQLITE3}" "${OUT}/restored.sqlite" ".read ${OUT}/dump.sql"
                RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR err)
    message(FATAL_ERROR "restoring the dump failed (${rc}): ${err}")
endif()
set(probe "SELECT id, hex(data), typeof(data) FROM files ORDER BY id; SELECT id, hex(name) FROM employees ORDER BY id;")
execute_process(COMMAND "${SQLITE3}" "${SRC}" "${probe}" OUTPUT_VARIABLE before)
execute_process(COMMAND "${SQLITE3}" "${OUT}/restored.sqlite" "${probe}" OUTPUT_VARIABLE after)
if(NOT before STREQUAL after OR before STREQUAL "")
    message(FATAL_ERROR "round trip changed the data\n--- original:\n${before}--- restored:\n${after}")
endif()
message(STATUS "dump round trip OK:\n${after}")
