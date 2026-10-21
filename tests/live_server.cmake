# Live-server selftest steps (tests/CMakeLists.txt, OPENYOG_TEST_MYSQL /
# OPENYOG_TEST_PG):
#   cmake -DMODE=mysql-fixture|mysql-roundtrip|pg-fixture|pg-roundtrip
#         -DSERVER=host:port:user:password:db -DCLI=<mysql or psql client>
#         [-DSQL=<fixture.sql>] [-DOPENYOG=<exe>] [-DOUT=<dir>] -P live_server.cmake
#
# *-fixture    create the database if needed and load the fixture into it
#              (PostgreSQL: also create <db>_other for --pgmultidbtest=)
# *-roundtrip  dump the database with openyog --dumpdb, restore the dump into
#              a scratch <db>_restore with the server's own client, compare
#              the employees rows byte for byte (BLOB and non-ASCII text
#              included), drop the scratch database
string(REPLACE ":" ";" parts "${SERVER}")
list(LENGTH parts n)
if(NOT n EQUAL 5)
    message(FATAL_ERROR "SERVER must be host:port:user:password:db, got '${SERVER}'")
endif()
list(GET parts 0 host)
list(GET parts 1 port)
list(GET parts 2 user)
list(GET parts 3 password)
list(GET parts 4 db)
set(restore "${db}_restore")

# run the server's client against `database`, SQL from `input` (a file) or
# `sql` (a string); fails the test on a non-zero exit, returns stdout
function(client database input sql result)
    if(MODE MATCHES "^mysql")
        set(cmd "${CMAKE_COMMAND}" -E env "MYSQL_PWD=${password}"
            "${CLI}" -h "${host}" -P "${port}" -u "${user}" --protocol=TCP -N -B)
    else()
        set(cmd "${CMAKE_COMMAND}" -E env "PGPASSWORD=${password}"
            "${CLI}" -h "${host}" -p "${port}" -U "${user}" -q -t -A -v ON_ERROR_STOP=1)
    endif()
    if(database)
        if(MODE MATCHES "^mysql")
            list(APPEND cmd "${database}")
        else()
            list(APPEND cmd -d "${database}")
        endif()
    elseif(MODE MATCHES "^pg")
        list(APPEND cmd -d postgres)
    endif()
    if(input)
        execute_process(COMMAND ${cmd} INPUT_FILE "${input}"
                        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    else()
        if(MODE MATCHES "^mysql")
            set(run -e) # mysql/mariadb: execute this
        else()
            set(run -c) # psql
        endif()
        execute_process(COMMAND ${cmd} ${run} "${sql}" RESULT_VARIABLE rc OUTPUT_VARIABLE out
                        ERROR_VARIABLE err)
    endif()
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${CLI} failed (${rc}) on '${database}': ${err}")
    endif()
    set(${result} "${out}" PARENT_SCOPE)
endfunction()

if(MODE STREQUAL "mysql-fixture")
    client("" "" "CREATE DATABASE IF NOT EXISTS `${db}` CHARACTER SET utf8mb4" ignored)
    client("${db}" "${SQL}" "" ignored)
    return()
endif()

if(MODE STREQUAL "pg-fixture")
    file(MAKE_DIRECTORY "${OUT}")
    file(WRITE "${OUT}/create-dbs.sql"
         "SELECT format('CREATE DATABASE %I', d) FROM unnest(ARRAY['${db}', '${db}_other']) d\n"
         "WHERE NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = d)\\gexec\n")
    client("" "${OUT}/create-dbs.sql" "" ignored)
    client("${db}" "${SQL}" "" ignored)
    # --pgmultidbtest= lists the other database's tables: give it one
    client("${db}_other" "" "CREATE TABLE IF NOT EXISTS other_t (id INTEGER PRIMARY KEY, note TEXT)"
           ignored)
    return()
endif()

# ---- round trips -----------------------------------------------------------
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
if(MODE STREQUAL "mysql-roundtrip")
    set(connect "--autoconnect=${SERVER}")
    set(probe "SELECT id, HEX(name), HEX(photo), salary, city FROM employees ORDER BY id")
    # the schema too: keys, indexes, foreign keys, as the server reports them
    set(schema_probe "SHOW CREATE TABLE employees")
else()
    set(connect "--autoconnectpg=${SERVER}")
    set(probe "SELECT id, encode(convert_to(name, 'UTF8'), 'hex'), encode(photo, 'hex'), salary, city FROM employees ORDER BY id")
    set(schema_probe "SELECT string_agg(x, E'\\n' ORDER BY x) FROM (SELECT conrelid::regclass || ' ' || conname || ' ' || pg_get_constraintdef(oid) AS x FROM pg_constraint WHERE connamespace = 'public'::regnamespace AND contype <> 'n' UNION ALL SELECT indexdef FROM pg_indexes WHERE schemaname = 'public') s")
endif()

execute_process(COMMAND "${OPENYOG}" "${connect}" "--dumpdb=${OUT}/dump.sql" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "openyog --dumpdb exited with ${rc}")
endif()

if(MODE STREQUAL "mysql-roundtrip")
    client("" "" "DROP DATABASE IF EXISTS `${restore}`; CREATE DATABASE `${restore}` CHARACTER SET utf8mb4" ignored)
else()
    client("" "" "DROP DATABASE IF EXISTS \"${restore}\"" ignored)
    client("" "" "CREATE DATABASE \"${restore}\"" ignored)
endif()
client("${restore}" "${OUT}/dump.sql" "" ignored)

client("${db}" "" "${probe}" before)
client("${restore}" "" "${probe}" after)
client("${db}" "" "${schema_probe}" schema_before)
client("${restore}" "" "${schema_probe}" schema_after)
set(failure "")
if(NOT before STREQUAL after OR before STREQUAL "")
    set(failure "round trip changed the data\n--- original:\n${before}--- restored:\n${after}")
elseif(NOT schema_before STREQUAL schema_after)
    set(failure "round trip changed the schema\n--- original:\n${schema_before}--- restored:\n${schema_after}")
endif()
# PostgreSQL: the serial's sequence has to sit past the restored ids
if(MODE STREQUAL "pg-roundtrip" AND NOT failure)
    client("${restore}" "" "SELECT MAX(id) + 1 FROM employees" expected)
    client("${restore}" "" "INSERT INTO employees (name) VALUES ('after restore') RETURNING id" got)
    if(NOT expected STREQUAL got)
        set(failure "after the restore the next serial id is ${got}, expected ${expected}")
    endif()
endif()

if(MODE STREQUAL "mysql-roundtrip")
    client("" "" "DROP DATABASE IF EXISTS `${restore}`" ignored)
else()
    client("" "" "DROP DATABASE IF EXISTS \"${restore}\"" ignored)
endif()
if(failure)
    message(FATAL_ERROR "${failure}")
endif()
message(STATUS "dump round trip OK:\n${after}")
