#include "SqliteConnection.h"

#include <QRegularExpression>

#include <algorithm>

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
     * in the query editor and for SqlDump's INSERT tuples. Wrong for BLOB
     * literals (SQLite wants X'hex', not an escaped quoted string) — nothing
     * routes SQLite BLOB data through here yet. */
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

/* PRAGMA <db?>.<pragma>(<arg>) — schema-qualified for non-default databases */
static QString pragma(const QString &db, const QString &stmt, const QString &arg)
{
    if(db.isEmpty() || db == QStringLiteral("main"))
        return QStringLiteral("PRAGMA %1(%2)").arg(stmt, arg);
    return QStringLiteral("PRAGMA %1.%2(%3)")
        .arg(QString(db).replace(QLatin1Char('"'), QStringLiteral("\"\"")), stmt, arg);
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
        : QStringLiteral("table");   /* "" and "BASE TABLE" both mean base */
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral("SELECT name FROM %1 WHERE type='%2'")
                       .arg(masterTable(db), type), &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(0);
    return out;
}

/* PRAGMA table_info rows: cid(0) name(1) type(2) notnull(3) dflt_value(4)
 * pk(5, 1-based position in the primary key). Rewrapped into the canonical
 * SHOW COLUMNS shape; a single-column INTEGER primary key is the rowid alias
 * and behaves exactly like a MySQL AUTO_INCREMENT, so it reports as such. */
DbResultSet SqliteConnection::listColumns(const QString &db, const QString &table)
{
    DbResultSet ti;
    runBuffered(pragma(db, QStringLiteral("table_info"), quoteIdent(table)), &ti, nullptr);

    int pkCols = 0;
    bool pkIsInteger = true;
    for(const QStringList &row : ti.rows) {
        if(row.value(5).toInt() > 0) {
            ++pkCols;
            if(!row.value(2).contains(QLatin1String("int"), Qt::CaseInsensitive))
                pkIsInteger = false;
        }
    }

    DbResultSet out;
    out.headers << QStringLiteral("Field") << QStringLiteral("Type")
                << QStringLiteral("Null") << QStringLiteral("Key")
                << QStringLiteral("Default") << QStringLiteral("Extra");
    for(const QStringList &row : ti.rows) {
        const bool isPk = row.value(5).toInt() > 0;
        QStringList r;
        r << row.value(1)                                     /* Field */
          << row.value(2)                                     /* Type */
          << (row.value(3).toInt() || isPk
                  ? QStringLiteral("NO") : QStringLiteral("YES"))  /* Null */
          << (isPk ? QStringLiteral("PRI") : QString())       /* Key */
          << row.value(4)                                     /* Default */
          << (isPk && pkCols == 1 && pkIsInteger
                  ? QStringLiteral("auto_increment") : QString());
        out.rows << r;
    }
    return out;
}

/* SHOW INDEX shape synthesized from PRAGMA index_list + index_info.
 * index_list rows: seq(0) name(1) unique(2) origin(3 'c'/'u'/'pk') partial(4);
 * index_info rows: seqno(0) cid(1) name(2). A primary key surfaces as a 'pk'
 * autoindex — except the single-column INTEGER rowid alias, which has no
 * index at all — so 'pk' autoindexes are reported as PRIMARY and PRIMARY is
 * synthesized from table_info whenever no such row was produced. */
DbResultSet SqliteConnection::listIndexes(const QString &db, const QString &table)
{
    DbResultSet out;
    out.headers << QStringLiteral("Table") << QStringLiteral("Non_unique")
                << QStringLiteral("Key_name") << QStringLiteral("Seq_in_index")
                << QStringLiteral("Column_name");
    const auto emitRow = [&](const QString &name, const QString &unique,
                             int seq, const QString &col) {
        QStringList r;
        r << table << unique << name << QString::number(seq) << col;
        out.rows << r;
    };

    DbResultSet il;
    runBuffered(pragma(db, QStringLiteral("index_list"), quoteIdent(table)), &il, nullptr);
    bool havePk = false;
    for(const QStringList &ix : il.rows) {
        /* a 'pk' autoindex IS the primary key — surface it under its real
         * name instead of the opaque sqlite_autoindex_* */
        const bool isPk = ix.value(3) == QStringLiteral("pk");
        havePk |= isPk;
        DbResultSet ii;
        runBuffered(pragma(db, QStringLiteral("index_info"), quoteIdent(ix.value(1))),
                    &ii, nullptr);
        /* index_list's unique flag inverts into SHOW INDEX's Non_unique */
        const QString nonUnique = ix.value(2) == QStringLiteral("1")
            ? QStringLiteral("0") : QStringLiteral("1");
        for(const QStringList &c : ii.rows)
            emitRow(isPk ? QStringLiteral("PRIMARY") : ix.value(1),
                    nonUnique, c.value(0).toInt() + 1, c.value(2));
    }

    /* synthesize PRIMARY from the table_info pk positions when the backend
     * produced none */
    if(!havePk) {
        QVector<QPair<int, QString>> pkCols;
        DbResultSet ti;
        runBuffered(pragma(db, QStringLiteral("table_info"), quoteIdent(table)),
                    &ti, nullptr);
        for(const QStringList &row : ti.rows) {
            const int pos = row.value(5).toInt();
            if(pos > 0)
                pkCols << qMakePair(pos, row.value(1));
        }
        std::sort(pkCols.begin(), pkCols.end());
        int seq = 1;
        for(const auto &pk : pkCols)
            emitRow(QStringLiteral("PRIMARY"), QStringLiteral("0"), seq++, pk.second);
    }
    return out;
}

/* PRAGMA foreign_key_list rows: id(0) seq(1) table(2) from(3) to(4)
 * on_update(5) on_delete(6) — one row per column of each constraint, in
 * declaration order. SQLite constraints are unnamed; a stable name is
 * synthesized (display + grouping only — SQLite has no DROP FOREIGN KEY). */
DbResultSet SqliteConnection::listForeignKeys(const QString &db, const QString &table)
{
    DbResultSet rs;
    runBuffered(pragma(db, QStringLiteral("foreign_key_list"), quoteIdent(table)),
                &rs, nullptr);

    DbResultSet out;
    out.headers << QStringLiteral("CONSTRAINT_NAME") << QStringLiteral("COLUMN_NAME")
                << QStringLiteral("REFERENCED_TABLE_NAME")
                << QStringLiteral("REFERENCED_COLUMN_NAME")
                << QStringLiteral("UPDATE_RULE") << QStringLiteral("DELETE_RULE");
    for(const QStringList &row : rs.rows) {
        QStringList r;
        r << QStringLiteral("FK_%1_%2").arg(row.value(2), row.value(0))
          << row.value(3) << row.value(2)
          << (row.value(4).isNull() ? QStringLiteral("NULL") : row.value(4))
          << row.value(5) << row.value(6);
        out.rows << r;
    }
    return out;
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

namespace {

/* unquote a SQLite identifier: "x" / `x` / [x] / bare */
QString unquoteIdent(QString s)
{
    if(s.size() >= 2
       && ((s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')))
           || (s.startsWith(QLatin1Char('`')) && s.endsWith(QLatin1Char('`')))
           || (s.startsWith(QLatin1Char('[')) && s.endsWith(QLatin1Char(']')))))
        return s.mid(1, s.size() - 2);
    return s;
}

} // namespace

/* canonical shape: Trigger(0) Timing(1 BEFORE/AFTER) Event(2 INSERT/UPDATE/
 * DELETE). SQLite keeps trigger DDL as free text in sqlite_master, so the
 * header is parsed back out ("ON <table>" filtered case-insensitively; an
 * absent timing means AFTER, per SQLite's grammar). */
DbResultSet SqliteConnection::listTableTriggers(const QString &db, const QString &table)
{
    DbResultSet out;
    out.headers << QStringLiteral("Trigger") << QStringLiteral("Timing")
                << QStringLiteral("Event");
    static const QRegularExpression re(
        QStringLiteral("\\b(BEFORE|AFTER|INSTEAD\\s+OF)?\\s*"
                       "(DELETE|INSERT|UPDATE)\\s+ON\\s+"
                       "((?:\"[^\"]+\")|(?:`[^`]+`)|(?:\\[[^\\]]+\\])|"
                       "[A-Za-z_][\\w$]*)"),
        QRegularExpression::CaseInsensitiveOption);
    DbResultSet rs;
    if(!runBuffered(QStringLiteral("SELECT name, sql FROM %1 WHERE type='trigger'")
                        .arg(masterTable(db)), &rs, nullptr))
        return out;
    for(const QStringList &row : rs.rows) {
        const auto m = re.match(row.value(1));
        if(!m.hasMatch())
            continue;
        if(unquoteIdent(m.captured(3)).compare(table, Qt::CaseInsensitive) != 0)
            continue;
        const QString timing = m.captured(1).isEmpty()
            ? QStringLiteral("AFTER")
            : m.captured(1).simplified().toUpper();
        QStringList r;
        r << row.value(0) << timing << m.captured(2).toUpper();
        out.rows << r;
    }
    return out;
}

DbResultSet SqliteConnection::listRoutines(const QString &)
{
    return {};   /* SQLite has no stored procedures/functions */
}

QStringList SqliteConnection::listEvents(const QString &)
{
    return {};   /* SQLite has no scheduled events */
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

QString SqliteConnection::sqlFkChecks(bool enable)
{
    return QStringLiteral("PRAGMA foreign_keys=%1").arg(enable ? QStringLiteral("ON")
                                                               : QStringLiteral("OFF"));
}

QString SqliteConnection::sqlSetNames(const QString &)
{
    return {};   /* no connection character-set concept */
}

QString SqliteConnection::sqlInsertDefaults(const QString &db, const QString &table)
{
    return QStringLiteral("INSERT INTO %1 DEFAULT VALUES")
        .arg(db.isEmpty() ? quoteIdent(table)
                          : quoteIdent(db) + QLatin1Char('.') + quoteIdent(table));
}

QString SqliteConnection::sqlTruncateTable(const QString &db, const QString &table)
{
    /* no TRUNCATE — and unlike MySQL, this does not reset the autoincrement
     * counter (that would need a second sqlite_sequence statement) */
    return QStringLiteral("DELETE FROM %1")
        .arg(db.isEmpty() ? quoteIdent(table)
                          : quoteIdent(db) + QLatin1Char('.') + quoteIdent(table));
}

bool SqliteConnection::supportsLimitOnUpdateDelete()
{
    /* SQLite only takes UPDATE/DELETE … LIMIT when compiled with
     * SQLITE_ENABLE_UPDATE_DELETE_LIMIT — can't assume the system lib has it */
    return false;
}
