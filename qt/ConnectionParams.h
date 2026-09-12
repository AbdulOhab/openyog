/* OpenYog — connection parameter type shared by
 * the connection dialog, the store and the connection tab. */
#pragma once

#include <QString>

/* SQLite/PostgreSQL are added here later — the value stays MySQL-only until
 * then, and old saved connections with no "driver" key default to it. */
enum class DriverType { Mysql = 0 };

struct ConnectionParams
{
    QString    name       = "New connection";
    DriverType driverType = DriverType::Mysql;
    QString    host       = "127.0.0.1";
    int        port       = 3306;
    QString    user;
    QString    password;
    QString    database;   // optional
};
