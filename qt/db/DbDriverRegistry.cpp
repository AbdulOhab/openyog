#include "DbDriverRegistry.h"
#include "MySqlDriver.h"
#include "SqliteDriver.h"

IDbDriver *dbDriverFor(DriverType type)
{
    static MySqlDriver mysql;
    static SqliteDriver sqlite;
    switch(type) {
        case DriverType::Mysql:  return &mysql;
        case DriverType::Sqlite: return &sqlite;
    }
    return nullptr;
}
