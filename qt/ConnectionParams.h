/* OpenYog — connection parameter type shared by
 * the connection dialog, the store and the connection tab. */
#pragma once

#include <QString>

/* PostgreSQL is added here later. */
enum class DriverType { Mysql = 0, Sqlite = 1 };

inline QString driverTypeToString(DriverType t)
{
    return t == DriverType::Sqlite ? QStringLiteral("sqlite") : QStringLiteral("mysql");
}

/* Unknown/missing strings fall back to `fallback` — keeps old saved
 * connections with no "driver" key (or a driver added by a newer build)
 * loading instead of failing outright. */
inline DriverType driverTypeFromString(const QString &s,
                                       DriverType fallback = DriverType::Mysql)
{
    if(s == QStringLiteral("sqlite")) return DriverType::Sqlite;
    if(s == QStringLiteral("mysql"))  return DriverType::Mysql;
    return fallback;
}

struct ConnectionParams
{
    QString    name       = "New connection";
    DriverType driverType = DriverType::Mysql;
    QString    host       = "127.0.0.1";
    int        port       = 3306;
    QString    user;
    QString    password;
    QString    database;   // optional
    QString    filePath;   // SQLite only: path to the .sqlite file
};
