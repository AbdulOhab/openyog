#include "DbDriverRegistry.h"
#include "MySqlDriver.h"
#include "SqliteDriver.h"
#include "PostgresDriver.h"

IDbDriver *dbDriverFor(SqlDriverType type)
{
    static MySqlDriver mysql;
    static SqliteDriver sqlite;
    static PostgresDriver postgres;
    switch(type) {
        case SqlDriverType::Mysql:
            return &mysql;
        case SqlDriverType::Sqlite:
            return &sqlite;
        case SqlDriverType::Postgres:
            return &postgres;
        case SqlDriverType::Mongo:
            break; /* not SQL: MongoClient, not an IDbDriver */
    }
    return nullptr;
}
