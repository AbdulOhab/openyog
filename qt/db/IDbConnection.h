/* OpenYog — database-connection abstraction. One live connection to one
 * server/file, backend-agnostic. Rows are plain value types (QStringList/
 * QByteArray), never a live cursor or backend handle, so a result can keep
 * crossing the qt/ThreadModel worker-thread boundary the way QueryResult
 * already does today. */
#pragma once

#include "../ConnectionParams.h"

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

/* Buffered result set: one query, fully materialized. */
struct DbResultSet
{
    QStringList headers;
    QVector<QStringList> rows;
};

class IDbConnection
{
public:
    virtual ~IDbConnection() = default;

    /* Which backend this is. Mostly for UI-level branching that has no
     * cheaper way to ask (e.g. the Object Browser's multi-database
     * Postgres support) — most dialect differences should still go through
     * the abstracted methods below, not a driverType() switch in caller code. */
    virtual SqlDriverType driverType() const = 0;

    /* Best-effort: ask a query currently running on this connection, from
     * ANOTHER thread, to stop. This is the one concurrent use this class
     * supports — never call query()/streamQuery() on the same IDbConnection
     * from two threads at once, only this alongside one of them. Safe to
     * call even if nothing is running (a harmless no-op then). */
    virtual void cancel() = 0;

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
    virtual bool streamQuery(const QString &sql, QString *error,
                             const std::function<void(const QStringList &headers)> &onHeaders,
                             const std::function<bool(const QVector<QByteArray> &fields,
                                                      const QVector<bool> &isNull)> &onRow) = 0;

    /* Escapes raw bytes for embedding in a single-quoted SQL literal (the
     * caller supplies the quotes). Does not add quoting itself. */
    virtual QByteArray escape(const QByteArray &raw) = 0;

    /* Quotes one identifier (db/table/column name) for safe embedding in SQL. */
    virtual QString quoteIdent(const QString &ident) = 0;

    /* db-qualified identifier, e.g. `db`.`table` — omits the schema entirely
     * when `db` is empty, rather than quoting it into an empty qualifier
     * (e.g. `` .`table` `` / "".table, both invalid SQL). `db` is legitimately
     * always empty for a SQLite file connection — it has no ConnectionParams::
     * database, only a filePath — so every db-qualified reference built from
     * `database.isEmpty() ? m_params.database : database`-style call sites
     * must go through this, not raw string formatting, to stay SQLite-safe. */
    QString qualify(const QString &db, const QString &name)
    {
        return db.isEmpty() ? quoteIdent(name)
                            : quoteIdent(db) + QLatin1Char('.') + quoteIdent(name);
    }

    virtual QString lastError() = 0;
    virtual qint64 affectedRows() = 0;
    virtual QString serverInfo() = 0;
    /* extra status text after DML (e.g. LOAD DATA's "Records: N  Deleted: N
     * Skipped: N  Warnings: N"); empty when the server has nothing to add. */
    virtual QString info() = 0;

    /* ---- metadata / DDL introspection --------------------------------
     * Every backend normalizes to these canonical shapes, so UI code never
     * sees dialect SQL or backend-native column orders:
     *   listColumns     — SHOW COLUMNS shape: Field(0) Type(1)
     *                     Null(2 "YES"/"NO") Key(3 "PRI"/"UNI"/"MUL"/"")
     *                     Default(4, the literal "NULL" when NULL)
     *                     Extra(5, may contain "auto_increment")
     *                     Comment(6, empty on backends with no column
     *                     comments, e.g. SQLite) — appended past the real
     *                     SHOW COLUMNS's 6 fields so existing callers that
     *                     only index up to Extra(5) are unaffected
     *   listIndexes     — SHOW INDEX shape: Non_unique(1 "0"/"1")
     *                     Key_name(2) Seq_in_index(3, 1-based) Column_name(4);
     *                     PRIMARY rows are always present (synthesized where
     *                     the backend has no explicit PRIMARY index, e.g. a
     *                     SQLite rowid-alias key)
     *   listForeignKeys — one row per (constraint, column) pair, ordered by
     *                     constraint then position: Name(0) Column(1)
     *                     Ref_table(2) Ref_column(3) On_update(4)
     *                     On_delete(5); unnamed constraints (SQLite) get a
     *                     stable synthesized name
     *   listTableTriggers — Trigger(0) Timing(1) Event(2), e.g.
     *                     "BEFORE" / "INSERT"
     * A backend that has no such objects returns an empty result (SQLite:
     * routines, events). `typeFilter`: "BASE TABLE" / "VIEW" / "" (= base). */
    virtual QStringList listDatabases() = 0;
    /* the physical databases reachable from this *server* (not just the one
     * this connection is attached to) — for MySQL/SQLite, identical to
     * listDatabases() (one connection already sees every database/the one
     * file); PostgreSQL overrides this, since listDatabases() there means
     * "this connection's own schemas" (a Postgres connection can't query
     * another database at all) while this means "every database on the
     * server," used by the Object Browser to show them all and lazily open
     * a side connection to any one the user actually expands. */
    virtual QStringList listPhysicalDatabases()
    {
        return listDatabases();
    }
    virtual QStringList listTables(const QString &db, const QString &typeFilter = {}) = 0;
    virtual DbResultSet listColumns(const QString &db, const QString &table) = 0;
    /* every distinct column name across every table/view in `db`, for
     * autocomplete only (no type/key info, so callers needing that still
     * go through listColumns() per table) — a backend with a system
     * catalog it can filter by schema in one query (Postgres, MySQL) should
     * override this instead of falling through to the default, which is a
     * blocking listColumns() call *per table* and was measured to dominate
     * ConnectionTab::switchDatabase()'s latency on any schema with more
     * than a handful of tables. */
    virtual QStringList listAllColumnNames(const QString &db)
    {
        QSet<QString> columns;
        for(const QString &t :
            listTables(db, QStringLiteral("BASE TABLE")) + listTables(db, QStringLiteral("VIEW")))
            for(const QStringList &row : listColumns(db, t).rows)
                if(!row.value(0).isEmpty())
                    columns.insert(row.value(0));
        return QStringList(columns.cbegin(), columns.cend());
    }
    virtual DbResultSet listIndexes(const QString &db, const QString &table) = 0;
    virtual DbResultSet listForeignKeys(const QString &db, const QString &table) = 0;
    virtual QStringList listTriggers(const QString &db) = 0;
    virtual DbResultSet listTableTriggers(const QString &db, const QString &table) = 0;
    virtual DbResultSet listRoutines(const QString &db) = 0;
    virtual QStringList listEvents(const QString &db) = 0;

    /* SHOW CREATE <kind> equivalent. `kind`: TABLE / VIEW / PROCEDURE /
     * FUNCTION / TRIGGER / EVENT. Returns the DDL text, or empty + *error set
     * (an unsupported kind on this backend is an error, not an empty DDL). */
    virtual QString showCreate(const QString &kind, const QString &db, const QString &name,
                               QString *error) = 0;

    /* ---- dialect fragments -------------------------------------------
     * The handful of statement shapes that differ between backends, so the
     * dump/edit/replay code in qt/ stays dialect-free. All return complete
     * statements (no trailing ';'; an empty string means "nothing to run"). */
    /* turn foreign-key enforcement off/on for this session */
    virtual QString sqlFkChecks(bool enable) = 0;
    /* set the connection character set ("SET NAMES …"); empty when the
     * backend has no such concept */
    virtual QString sqlSetNames(const QString &charset) = 0;
    /* insert one row taking every column default */
    virtual QString sqlInsertDefaults(const QString &db, const QString &table) = 0;
    /* "empty this table" (MySQL TRUNCATE; SQLite DELETE FROM) */
    virtual QString sqlTruncateTable(const QString &db, const QString &table) = 0;
    /* does the backend accept `UPDATE/DELETE … LIMIT n`? (SQLite only with
     * SQLITE_ENABLE_UPDATE_DELETE_LIMIT — assume no) */
    virtual bool supportsLimitOnUpdateDelete() = 0;
};
