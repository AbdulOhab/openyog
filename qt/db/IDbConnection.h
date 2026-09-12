/* OpenYog — database-connection abstraction. One live connection to one
 * server/file, backend-agnostic. Rows are plain value types (QStringList/
 * QByteArray), never a live cursor or backend handle, so a result can keep
 * crossing the qt/ThreadModel worker-thread boundary the way QueryResult
 * already does today. */
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

/* Buffered result set: one query, fully materialized. */
struct DbResultSet
{
    QStringList          headers;
    QVector<QStringList> rows;
};

class IDbConnection
{
public:
    virtual ~IDbConnection() = default;

    /* Buffered execute. `result` may be null when the caller only wants the
     * status message (DDL/DML). `message` always gets a human status line
     * ("N row(s) in result set" / "OK, N row(s) affected"). Returns false
     * and fills `message` with the error on failure. */
    virtual bool query(const QString &sql, DbResultSet *result, QString *message) = 0;

    /* Streaming execute for full-table reads (e.g. SQL dump): fields arrive
     * as raw bytes, not QString, so binary/BLOB columns survive escape()
     * unmodified. `onRow` returning false aborts the fetch early. Returns
     * false and fills `error` on failure (including a mid-stream abort by
     * `onRow` returning false is NOT an error — it's a normal early stop). */
    virtual bool streamQuery(
        const QString &sql, QString *error,
        const std::function<void(const QStringList &headers)> &onHeaders,
        const std::function<bool(const QVector<QByteArray> &fields,
                                  const QVector<bool> &isNull)> &onRow) = 0;

    /* Escapes raw bytes for embedding in a single-quoted SQL literal (the
     * caller supplies the quotes). Does not add quoting itself. */
    virtual QByteArray escape(const QByteArray &raw) = 0;

    /* Quotes one identifier (db/table/column name) for safe embedding in SQL. */
    virtual QString quoteIdent(const QString &ident) = 0;

    virtual QString lastError() = 0;
    virtual qint64  affectedRows() = 0;
    virtual QString serverInfo() = 0;

    /* ---- metadata / DDL introspection -------------------------------- */
    virtual QStringList listDatabases() = 0;
    virtual QStringList listTables(const QString &db, const QString &typeFilter = {}) = 0;
    virtual DbResultSet listColumns(const QString &db, const QString &table) = 0;
    virtual DbResultSet listIndexes(const QString &db, const QString &table) = 0;
    virtual DbResultSet listForeignKeys(const QString &db, const QString &table) = 0;
    virtual QStringList listTriggers(const QString &db) = 0;
    virtual DbResultSet listRoutines(const QString &db) = 0;

    /* SHOW CREATE <kind> equivalent. `kind`: TABLE / VIEW / PROCEDURE /
     * FUNCTION / TRIGGER / EVENT. Returns the DDL text, or empty + *error set. */
    virtual QString showCreate(const QString &kind, const QString &db,
                               const QString &name, QString *error) = 0;
};
