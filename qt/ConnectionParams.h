/* OpenYog — connection parameter type shared by
 * the connection dialog, the store and the connection tab. */
#pragma once

#include <QString>
#include <QUrl>

/* Mongo isn't SQL: it opens a MongoTab, never a ConnectionTab, and none of
 * the SQL machinery (IDbConnection and what's built on it) sees it */
enum class SqlDriverType { Mysql = 0, Sqlite = 1, Postgres = 2, Mongo = 3 };

inline QString driverTypeToString(SqlDriverType t)
{
    switch(t) {
        case SqlDriverType::Sqlite:
            return QStringLiteral("sqlite");
        case SqlDriverType::Postgres:
            return QStringLiteral("postgres");
        case SqlDriverType::Mongo:
            return QStringLiteral("mongodb");
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
        case SqlDriverType::Mongo:
            return QStringLiteral("MongoDB");
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
    if(s == QStringLiteral("mongodb"))
        return SqlDriverType::Mongo;
    return fallback;
}

/* a MongoDB connection string's user:password@ taken out (percent-decoded
 * into *user and *password, when there is one), so the password is kept
 * like any other instead of in the saved string */
inline QString mongoUriWithoutCredentials(const QString &uri, QString *user, QString *password)
{
    const QString u = uri.trimmed();
    const qsizetype scheme = u.indexOf(QStringLiteral("://"));
    if(scheme < 0)
        return u;
    const qsizetype hostsEnd = u.indexOf(QLatin1Char('/'), scheme + 3);
    const qsizetype at = u.lastIndexOf(QLatin1Char('@'), hostsEnd < 0 ? qsizetype(-1) : hostsEnd);
    if(at < scheme + 3)
        return u;
    const QString info = u.mid(scheme + 3, at - scheme - 3);
    const qsizetype colon = info.indexOf(QLatin1Char(':'));
    *user = QUrl::fromPercentEncoding(info.left(colon < 0 ? info.size() : colon).toUtf8());
    if(colon >= 0)
        *password = QUrl::fromPercentEncoding(info.mid(colon + 1).toUtf8());
    return u.left(scheme + 3) + u.mid(at + 1);
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

    // MongoDB only: a connection string (mongodb:// or mongodb+srv://) used
    // instead of host/port, without credentials (user/password above go
    // with it); and the database the user is defined in (authSource)
    QString mongoUri;
    QString authDatabase;

    // MySQL/PostgreSQL: reach the server through an SSH tunnel (SshTunnel) —
    // host/port above are then the database as seen *from the SSH host*
    bool useSsh = false;
    QString sshHost;
    int sshPort = 22;
    QString sshUser;
    QString sshPassword; // login password, or the private key's passphrase
    QString sshKeyFile;  // private key; empty: password, then ssh-agent / ~/.ssh keys

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
