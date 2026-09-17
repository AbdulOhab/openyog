/* OpenYog — SQLite IDbConnection implementation. Everything above the driver
 * seam (object browser, table-data editing, SQL dump) goes through the
 * canonical metadata shapes, which this class synthesizes from the PRAGMA
 * introspection + sqlite_master (the UI never sees SQLite dialect SQL). */
#pragma once

#include "IDbConnection.h"

#include <sqlite3.h>

class SqliteConnection : public IDbConnection
{
public:
    /* Takes ownership of an already-open handle; closes it on destruction. */
    explicit SqliteConnection(sqlite3 *db);
    ~SqliteConnection() override;

    SqlDriverType driverType() const override
    {
        return SqlDriverType::Sqlite;
    }

    void cancel() override;
    bool query(const QString &sql, DbResultSet *result, QString *message) override;

    bool streamQuery(const QString &sql, QString *error,
                     const std::function<void(const QStringList &headers)> &onHeaders,
                     const std::function<bool(const QVector<QByteArray> &fields,
                                              const QVector<bool> &isNull)> &onRow) override;

    QByteArray escape(const QByteArray &raw) override;
    QString quoteIdent(const QString &ident) override;
    QString lastError() override;
    qint64 affectedRows() override;
    QString serverInfo() override;
    QString info() override;

    QStringList listDatabases() override;
    QStringList listTables(const QString &db, const QString &typeFilter) override;
    DbResultSet listColumns(const QString &db, const QString &table) override;
    DbResultSet listIndexes(const QString &db, const QString &table) override;
    DbResultSet listForeignKeys(const QString &db, const QString &table) override;
    QStringList listTriggers(const QString &db) override;
    DbResultSet listTableTriggers(const QString &db, const QString &table) override;
    DbResultSet listRoutines(const QString &db) override;
    QStringList listEvents(const QString &db) override;
    QString showCreate(const QString &kind, const QString &db, const QString &name,
                       QString *error) override;

    QString sqlFkChecks(bool enable) override;
    QString sqlSetNames(const QString &charset) override;
    QString sqlInsertDefaults(const QString &db, const QString &table) override;
    QString sqlTruncateTable(const QString &db, const QString &table) override;
    bool supportsLimitOnUpdateDelete() override;

private:
    /* prepares `sql`, runs it to completion, buffering rows into `out` if
     * given. `out` may be null (DDL/DML — caller only wants success/error). */
    bool runBuffered(const QString &sql, DbResultSet *out, QString *error);
    /* schema-qualifies `sqlite_master` for non-default (ATTACHed) databases */
    static QString masterTable(const QString &db);

    sqlite3 *m_db;
};
