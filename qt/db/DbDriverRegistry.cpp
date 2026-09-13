#include "DbDriverRegistry.h"
#include "MySqlDriver.h"
#include "SqliteDriver.h"
#include "PostgresDriver.h"

IDbDriver *dbDriverFor(DriverType type)
{
    static MySqlDriver mysql;
    static SqliteDriver sqlite;
    static PostgresDriver postgres;
    switch(type) {
        case DriverType::Mysql:    return &mysql;
        case DriverType::Sqlite:   return &sqlite;
        case DriverType::Postgres: return &postgres;
    }
    return nullptr;
}
