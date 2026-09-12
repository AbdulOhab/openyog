#include "SqliteConnection.h"

SqliteConnection::SqliteConnection(sqlite3 *db)
    : m_db(db)
{
}

SqliteConnection::~SqliteConnection()
{
    if(m_db)
        sqlite3_close(m_db);
}

bool SqliteConnection::runBuffered(const QString &sql, DbResultSet *out, QString *error)
{
    sqlite3_stmt *stmt = nullptr;
    const QByteArray utf8 = sql.toUtf8();
    if(sqlite3_prepare_v2(m_db, utf8.constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        if(error) *error = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    const int n = sqlite3_column_count(stmt);
    if(out && n > 0)
        for(int i = 0; i < n; ++i)
            out->headers << QString::fromUtf8(sqlite3_column_name(stmt, i));

    int rc;
    while((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if(!out)
            continue;
        QStringList row;
        for(int i = 0; i < n; ++i) {
            if(sqlite3_column_type(stmt, i) == SQLITE_NULL) {
                row << QStringLiteral("NULL");
                continue;
            }
            row << QString::fromUtf8(
                reinterpret_cast<const char *>(sqlite3_column_text(stmt, i)),
                sqlite3_column_bytes(stmt, i));
        }
        out->rows << row;
    }
    const bool ok = (rc == SQLITE_DONE);
    if(!ok && error)
        *error = QString::fromUtf8(sqlite3_errmsg(m_db));
    sqlite3_finalize(stmt);
    return ok;
}

bool SqliteConnection::query(const QString &sql, DbResultSet *result, QString *message)
{
    DbResultSet local;
    DbResultSet *target = result ? result : &local;
    if(!runBuffered(sql, target, message))
        return false;
    if(message) {
        *message = target->headers.isEmpty()
            ? QStringLiteral("OK, %1 row(s) affected").arg(affectedRows())
            : QStringLiteral("%1 row(s) in result set").arg(target->rows.size());
    }
    return true;
}

bool SqliteConnection::streamQuery(
    const QString &sql, QString *error,
    const std::function<void(const QStringList &headers)> &onHeaders,
    const std::function<bool(const QVector<QByteArray> &fields,
                              const QVector<bool> &isNull)> &onRow)
{
    sqlite3_stmt *stmt = nullptr;
    const QByteArray utf8 = sql.toUtf8();
    if(sqlite3_prepare_v2(m_db, utf8.constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        if(error) *error = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    const int n = sqlite3_column_count(stmt);
    if(onHeaders) {
        QStringList headers;
        for(int i = 0; i < n; ++i)
            headers << QString::fromUtf8(sqlite3_column_name(stmt, i));
        onHeaders(headers);
    }

    int rc;
    bool stopped = false;
    while((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        QVector<QByteArray> fields(n);
        QVector<bool> isNull(n);
        for(int i = 0; i < n; ++i) {
            isNull[i] = sqlite3_column_type(stmt, i) == SQLITE_NULL;
            if(!isNull[i])
                fields[i] = QByteArray(
                    reinterpret_cast<const char *>(sqlite3_column_blob(stmt, i)),
                    sqlite3_column_bytes(stmt, i));
        }
        if(onRow && !onRow(fields, isNull)) {
            stopped = true;
            break;
        }
    }
    const bool queryFailed = !stopped && rc != SQLITE_DONE;
    if(queryFailed && error)
        *error = QString::fromUtf8(sqlite3_errmsg(m_db));
    sqlite3_finalize(stmt);
    return !queryFailed;
}

QByteArray SqliteConnection::escape(const QByteArray &raw)
{
    /* SQL-standard single-quote doubling — correct for TEXT literals typed
     * in the query editor. Wrong for BLOB literals (SQLite wants X'hex',
     * not an escaped quoted string) but nothing routes BLOB data through
     * this yet (SqlDump isn't wired to the SQLite driver in this pass). */
    QByteArray out;
    out.reserve(raw.size() + 8);
    for(char c : raw) {
        if(c == '\'')
            out.append('\'');
        out.append(c);
    }
    return out;
}

QString SqliteConnection::quoteIdent(const QString &ident)
{
    return QLatin1Char('"') + QString(ident).replace(QLatin1Char('"'), QStringLiteral("\"\""))
         + QLatin1Char('"');
}

QString SqliteConnection::lastError() { return QString::fromUtf8(sqlite3_errmsg(m_db)); }
qint64  SqliteConnection::affectedRows() { return (qint64)sqlite3_changes(m_db); }
QString SqliteConnection::serverInfo() { return QString::fromUtf8(sqlite3_libversion()); }
QString SqliteConnection::info() { return {}; }   /* no mysql_info() equivalent */

QString SqliteConnection::masterTable(const QString &db)
{
    if(db.isEmpty() || db == QStringLiteral("main"))
        return QStringLiteral("sqlite_master");
    return QStringLiteral("\"%1\".sqlite_master")
        .arg(QString(db).replace(QLatin1Char('"'), QStringLiteral("\"\"")));
}

QStringList SqliteConnection::listDatabases()
{
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral("PRAGMA database_list"), &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(1);   /* seq(0), name(1), file(2) */
    return out;
}

QStringList SqliteConnection::listTables(const QString &db, const QString &typeFilter)
{
    const QString type = typeFilter.compare(QStringLiteral("VIEW"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("view")
        : (typeFilter.isEmpty()
               || typeFilter.compare(QStringLiteral("BASE TABLE"), Qt::CaseInsensitive) == 0)
              ? QStringLiteral("table")
              : typeFilter.toLower();
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral("SELECT name FROM %1 WHERE type='%2'")
                       .arg(masterTable(db), type), &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(0);
    return out;
}

DbResultSet SqliteConnection::listColumns(const QString &db, const QString &table)
{
    DbResultSet rs;
    const QString stmt = (db.isEmpty() || db == QStringLiteral("main"))
        ? QStringLiteral("PRAGMA table_info(%1)").arg(quoteIdent(table))
        : QStringLiteral("PRAGMA %1.table_info(%2)").arg(quoteIdent(db), quoteIdent(table));
    runBuffered(stmt, &rs, nullptr);
    return rs;
}

DbResultSet SqliteConnection::listIndexes(const QString &db, const QString &table)
{
    DbResultSet rs;
    const QString stmt = (db.isEmpty() || db == QStringLiteral("main"))
        ? QStringLiteral("PRAGMA index_list(%1)").arg(quoteIdent(table))
        : QStringLiteral("PRAGMA %1.index_list(%2)").arg(quoteIdent(db), quoteIdent(table));
    runBuffered(stmt, &rs, nullptr);
    return rs;
}

DbResultSet SqliteConnection::listForeignKeys(const QString &db, const QString &table)
{
    DbResultSet rs;
    const QString stmt = (db.isEmpty() || db == QStringLiteral("main"))
        ? QStringLiteral("PRAGMA foreign_key_list(%1)").arg(quoteIdent(table))
        : QStringLiteral("PRAGMA %1.foreign_key_list(%2)").arg(quoteIdent(db), quoteIdent(table));
    runBuffered(stmt, &rs, nullptr);
    return rs;
}

QStringList SqliteConnection::listTriggers(const QString &db)
{
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral("SELECT name FROM %1 WHERE type='trigger'")
                       .arg(masterTable(db)), &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(0);
    return out;
}

DbResultSet SqliteConnection::listRoutines(const QString &)
{
    return {};   /* SQLite has no stored procedures/functions */
}

QString SqliteConnection::showCreate(const QString &kind, const QString &db,
                                     const QString &name, QString *error)
{
    const QString k = kind.toUpper();
    if(k != QStringLiteral("TABLE") && k != QStringLiteral("VIEW")
       && k != QStringLiteral("TRIGGER")) {
        if(error) *error = QStringLiteral("not supported on SQLite");
        return {};
    }
    DbResultSet rs;
    const QString sql = QStringLiteral("SELECT sql FROM %1 WHERE type='%2' AND name='%3'")
        .arg(masterTable(db), k.toLower(), QString(name).replace('\'', QStringLiteral("''")));
    if(!runBuffered(sql, &rs, error) || rs.rows.isEmpty())
        return {};
    return rs.rows.first().value(0);
}
