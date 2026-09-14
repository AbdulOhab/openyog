/* OpenYog — PostgreSQL IDbConnection implementation (libpq).
 *
 * One fundamental mismatch with the MySQL/SQLite backends: a libpq
 * connection is to exactly one Postgres *database*, and there is no
 * equivalent of MySQL's "SHOW TABLES FROM otherdb" or SQLite's ATTACH —
 * browsing a different Postgres database means a whole new connection.
 * The natural sub-unit that *is* browsable within one connection is the
 * schema, so every `db` parameter below means "schema", and
 * listDatabases() returns this connection's schemas (minus the system
 * ones), not other Postgres databases. This is documented again at
 * listDatabases()'s definition, since it's the one place the seam's
 * naming ("database") and Postgres's actual model genuinely diverge.
 *
 * Consequence worth knowing: the object browser tree is unaffected (each
 * node carries the real schema name it was listed under), but the handful
 * of ConnectionTab prompt*() methods that fall back to
 * `ConnectionParams::database` when no explicit db/schema was passed in
 * (Create Table with nothing selected, Copy/Alter Database, etc.) will
 * fall back to the *connected database name*, not "public" — those still
 * need a schema-aware default before they're correct for Postgres. Not
 * fixed here, same as several other subsystems plan.md already declares
 * MySQL-only by design (Copy-Database internals, Alter Table seeding,
 * charset/collation reads, LOAD DATA/XML import, User Manager). */
#pragma once

#include "IDbConnection.h"

#include <libpq-fe.h>

class PostgresConnection : public IDbConnection
{
public:
    /* Takes ownership of an already-connected handle; closes it on
     * destruction. */
    explicit PostgresConnection(PGconn *conn);
    ~PostgresConnection() override;

    DriverType driverType() const override { return DriverType::Postgres; }

    void cancel() override;
    bool query(const QString &sql, DbResultSet *result, QString *message) override;

    bool streamQuery(
        const QString &sql, QString *error,
        const std::function<void(const QStringList &headers)> &onHeaders,
        const std::function<bool(const QVector<QByteArray> &fields,
                                  const QVector<bool> &isNull)> &onRow) override;
    /* every real database on the server, not just this connection's own
     * schemas (see IDbConnection::listPhysicalDatabases()'s doc comment) */
    QStringList listPhysicalDatabases() override;

    QByteArray escape(const QByteArray &raw) override;
    QString    quoteIdent(const QString &ident) override;
    QString    lastError() override;
    qint64     affectedRows() override;
    QString    serverInfo() override;
    QString    info() override;

    QStringList listDatabases() override;
    QStringList listTables(const QString &db, const QString &typeFilter) override;
    DbResultSet listColumns(const QString &db, const QString &table) override;
    DbResultSet listIndexes(const QString &db, const QString &table) override;
    DbResultSet listForeignKeys(const QString &db, const QString &table) override;
    QStringList listTriggers(const QString &db) override;
    DbResultSet listTableTriggers(const QString &db, const QString &table) override;
    DbResultSet listRoutines(const QString &db) override;
    QStringList listEvents(const QString &db) override;
    QString     showCreate(const QString &kind, const QString &db,
                           const QString &name, QString *error) override;

    QString sqlFkChecks(bool enable) override;
    QString sqlSetNames(const QString &charset) override;
    QString sqlInsertDefaults(const QString &db, const QString &table) override;
    QString sqlTruncateTable(const QString &db, const QString &table) override;
    bool    supportsLimitOnUpdateDelete() override;

private:
    /* runs `sql`, buffers rows into `out` (may be null for DDL/DML); tracks
     * affected-row count from PQcmdTuples() for affectedRows() to read back
     * later, since libpq has no MySQL-style "affected rows on this
     * connection" call — it only comes from the result of the query that
     * produced it. */
    bool runBuffered(const QString &sql, DbResultSet *out, QString *error);
    /* the schema to introspect against when the caller passes an empty one
     * (mirrors m_params.database's role for MySQL/SQLite, but here it's
     * always "public" — Postgres's own default schema — never empty) */
    static QString effectiveSchema(const QString &db);

    PGconn *m_conn;
    qint64  m_lastAffected = 0;
};
