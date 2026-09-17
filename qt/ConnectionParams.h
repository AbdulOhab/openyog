/* OpenYog — connection parameter type shared by
 * the connection dialog, the store and the connection tab. */
#pragma once

#include <QString>

enum class SqlDriverType { Mysql = 0, Sqlite = 1, Postgres = 2 };

inline QString driverTypeToString(SqlDriverType t)
{
    switch(t) {
        case SqlDriverType::Sqlite:
            return QStringLiteral("sqlite");
        case SqlDriverType::Postgres:
            return QStringLiteral("postgres");
        case SqlDriverType::Mysql:
            break;
    }
    return QStringLiteral("mysql");
}

/* human-readable driver name for the UI (status bar, dialogs) —
 * driverTypeToString() above is the lowercase persistence form */
inline QString driverDisplayName(SqlDriverType t)
{
    switch(t) {
        case SqlDriverType::Mysql:
            return QStringLiteral("MySQL");
        case SqlDriverType::Sqlite:
            return QStringLiteral("SQLite");
        case SqlDriverType::Postgres:
            return QStringLiteral("PostgreSQL");
    }
    return QStringLiteral("MySQL");
}

/* Unknown/missing strings fall back to `fallback` — keeps old saved
 * connections with no "driver" key (or a driver added by a newer build)
 * loading instead of failing outright. */
inline SqlDriverType driverTypeFromString(const QString &s,
                                          SqlDriverType fallback = SqlDriverType::Mysql)
{
    if(s == QStringLiteral("sqlite"))
        return SqlDriverType::Sqlite;
    if(s == QStringLiteral("postgres"))
        return SqlDriverType::Postgres;
    if(s == QStringLiteral("mysql"))
        return SqlDriverType::Mysql;
    return fallback;
}

struct ConnectionParams
{
    QString name = "New connection";
    SqlDriverType driverType = SqlDriverType::Mysql;
    QString host = "127.0.0.1";
    int port = 3306;
    QString user;
    QString password;
    QString database; // optional
    QString filePath; // SQLite only: path to the .sqlite file

    // MySQL/PostgreSQL: client-cert TLS (mysql_ssl_set / libpq sslmode=verify-*)
    bool useSsl = false;
    QString sslCa;   // CA cert
    QString sslCert; // client cert
    QString sslKey;  // client key

    // MySQL only — libpq has no protocol-compression option for a plain
    // TCP connection, so there's no PostgreSQL equivalent to wire this to
    bool compress = false; // mysql_options(MYSQL_OPT_COMPRESS)

    // MySQL: 0 = server default, else SET SESSION wait_timeout
    // PostgreSQL: 0 = server default, else SET idle_session_timeout (PG 14+;
    //   a best-effort SET on connect, same as MySQL's — an older server
    //   that doesn't recognize the GUC just keeps its own default)
    int idleTimeoutSecs = 0;

    // MySQL: 0 = disabled, else a periodic no-op ping (SELECT 1) on the
    //   browsing connection
    // PostgreSQL: 0 = disabled, else real TCP keepalives via libpq's own
    //   keepalives/keepalives_idle conninfo options — no app-level ping
    //   needed, the OS socket handles it
    int keepAliveSecs = 0;
};
