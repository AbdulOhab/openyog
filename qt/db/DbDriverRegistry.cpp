#include "DbDriverRegistry.h"
#include "MySqlDriver.h"

IDbDriver *dbDriverFor(DriverType type)
{
    static MySqlDriver mysql;
    switch(type) {
        case DriverType::Mysql: return &mysql;
    }
    return nullptr;
}
