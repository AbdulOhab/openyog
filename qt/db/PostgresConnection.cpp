#include "PostgresConnection.h"

#include <QHash>
#include <QSet>

PostgresConnection::PostgresConnection(PGconn *conn)
    : m_conn(conn)
{
}

PostgresConnection::~PostgresConnection()
{
    if(m_conn)
        PQfinish(m_conn);
}

void PostgresConnection::cancel()
{
    /* PQgetCancel()/PQcancel() are libpq's own documented thread-safe cancel
     * API — unlike MySQL (which needs a throwaway auxiliary connection to
     * send KILL QUERY, since the busy connection can't process anything
     * else while a query runs), this talks to the server on a side channel
     * libpq sets up for exactly this purpose. Safe to call with nothing
     * running (PQcancel then just fails silently server-side). */
    if(!m_conn)
        return;
    PGcancel *c = PQgetCancel(m_conn);
    if(!c)
        return;
    char errbuf[256];
    PQcancel(c, errbuf, sizeof(errbuf));
    PQfreeCancel(c);
}

bool PostgresConnection::runBuffered(const QString &sql, DbResultSet *out, QString *error)
{
    const QByteArray utf8 = sql.toUtf8();
    PGresult *res = PQexec(m_conn, utf8.constData());
    const ExecStatusType st = PQresultStatus(res);

    if(st == PGRES_COMMAND_OK) {
        m_lastAffected = QByteArray(PQcmdTuples(res)).toLongLong();
        PQclear(res);
        return true;
    }
    if(st == PGRES_TUPLES_OK) {
        m_lastAffected = 0;
        if(out) {
            const int nf = PQnfields(res);
            const int nr = PQntuples(res);
            for(int i = 0; i < nf; ++i)
                out->headers << QString::fromUtf8(PQfname(res, i));
            for(int r = 0; r < nr; ++r) {
                QStringList row;
                for(int i = 0; i < nf; ++i)
                    row << (PQgetisnull(res, r, i)
                                ? QStringLiteral("NULL")
                                : QString::fromUtf8(PQgetvalue(res, r, i)));
                out->rows << row;
            }
        }
        PQclear(res);
        return true;
    }
    if(error) *error = QString::fromUtf8(PQresultErrorMessage(res));
    PQclear(res);
    return false;
}

bool PostgresConnection::query(const QString &sql, DbResultSet *result, QString *message)
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

bool PostgresConnection::streamQuery(
    const QString &sql, QString *error,
    const std::function<void(const QStringList &headers)> &onHeaders,
    const std::function<bool(const QVector<QByteArray> &fields,
                              const QVector<bool> &isNull)> &onRow)
{
    const QByteArray utf8 = sql.toUtf8();
    if(!PQsendQuery(m_conn, utf8.constData())) {
        if(error) *error = QString::fromUtf8(PQerrorMessage(m_conn));
        return false;
    }
    /* one-row-at-a-time results instead of PQexec's fully-buffered set —
     * needed for the same reason MySqlConnection uses mysql_use_result
     * instead of mysql_store_result: a full-table dump shouldn't have to
     * fit in memory twice (once in libpq's buffer, once in ours). */
    PQsetSingleRowMode(m_conn);

    bool headersSent = false;
    bool stopped = false;
    bool failed = false;
    PGresult *res;
    while((res = PQgetResult(m_conn)) != nullptr) {
        const ExecStatusType st = PQresultStatus(res);
        if(st == PGRES_SINGLE_TUPLE || st == PGRES_TUPLES_OK) {
            const int nf = PQnfields(res);
            if(!headersSent) {
                if(onHeaders) {
                    QStringList headers;
                    for(int i = 0; i < nf; ++i)
                        headers << QString::fromUtf8(PQfname(res, i));
                    onHeaders(headers);
                }
                headersSent = true;
            }
            if(st == PGRES_SINGLE_TUPLE && !stopped) {
                QVector<QByteArray> fields(nf);
                QVector<bool> isNull(nf);
                for(int i = 0; i < nf; ++i) {
                    isNull[i] = PQgetisnull(res, 0, i);
                    if(!isNull[i])
                        fields[i] = QByteArray(PQgetvalue(res, 0, i), PQgetlength(res, 0, i));
                }
                if(onRow && !onRow(fields, isNull))
                    stopped = true;   /* keep draining libpq's queue below */
            }
        } else if(st == PGRES_FATAL_ERROR) {
            if(error) *error = QString::fromUtf8(PQresultErrorMessage(res));
            failed = true;
        }
        PQclear(res);
    }
    return !failed;
}

QByteArray PostgresConnection::escape(const QByteArray &raw)
{
    QByteArray out(raw.size() * 2 + 1, '\0');
    int errorFlag = 0;
    const size_t n = PQescapeStringConn(m_conn, out.data(), raw.constData(),
                                        size_t(raw.size()), &errorFlag);
    out.resize(int(n));
    return out;
}

QString PostgresConnection::quoteIdent(const QString &ident)
{
    const QByteArray utf8 = ident.toUtf8();
    char *escaped = PQescapeIdentifier(m_conn, utf8.constData(), size_t(utf8.size()));
    /* PQescapeIdentifier already wraps the result in double quotes */
    const QString result = escaped ? QString::fromUtf8(escaped)
                                   : QLatin1Char('"') + ident + QLatin1Char('"');
    if(escaped) PQfreemem(escaped);
    return result;
}

QString PostgresConnection::lastError() { return QString::fromUtf8(PQerrorMessage(m_conn)); }
qint64  PostgresConnection::affectedRows() { return m_lastAffected; }

QString PostgresConnection::serverInfo()
{
    const char *v = PQparameterStatus(m_conn, "server_version");
    return v ? QString::fromUtf8(v) : QString::number(PQserverVersion(m_conn));
}

QString PostgresConnection::info() { return {}; }   /* no mysql_info() equivalent */

QString PostgresConnection::effectiveSchema(const QString &db)
{
    return db.isEmpty() ? QStringLiteral("public") : db;
}

/* Postgres has no cross-database browsing on one connection (see the class
 * doc comment) — what upstream calls "databases" here are this connection's
 * own schemas, filtering out the system ones nobody wants to see in the
 * object browser. */
QStringList PostgresConnection::listDatabases()
{
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral(
           "SELECT schema_name FROM information_schema.schemata "
           "WHERE schema_name NOT IN ('pg_catalog','information_schema') "
           "AND schema_name NOT LIKE 'pg\\_toast%' "
           "AND schema_name NOT LIKE 'pg\\_temp%' "
           "ORDER BY schema_name"), &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(0);
    return out;
}

QStringList PostgresConnection::listTables(const QString &db, const QString &typeFilter)
{
    const QString type = typeFilter.compare(QStringLiteral("VIEW"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("VIEW") : QStringLiteral("BASE TABLE");
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral(
           "SELECT table_name FROM information_schema.tables "
           "WHERE table_schema='%1' AND table_type='%2' ORDER BY table_name")
               .arg(effectiveSchema(db), type), &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(0);
    return out;
}

DbResultSet PostgresConnection::listColumns(const QString &db, const QString &table)
{
    const QString schema = effectiveSchema(db);
    DbResultSet cols;
    runBuffered(QStringLiteral(
        "SELECT column_name, "
        "  CASE WHEN data_type='character varying' AND character_maximum_length IS NOT NULL "
        "         THEN 'varchar(' || character_maximum_length || ')' "
        "       WHEN data_type='character' AND character_maximum_length IS NOT NULL "
        "         THEN 'char(' || character_maximum_length || ')' "
        "       WHEN data_type='numeric' AND numeric_precision IS NOT NULL "
        "         THEN 'numeric(' || numeric_precision || ',' || numeric_scale || ')' "
        "       ELSE data_type END, "
        "  is_nullable, column_default, is_identity "
        "FROM information_schema.columns "
        "WHERE table_schema='%1' AND table_name='%2' ORDER BY ordinal_position")
            .arg(schema, table), &cols, nullptr);

    DbResultSet pk;
    runBuffered(QStringLiteral(
        "SELECT kcu.column_name FROM information_schema.table_constraints tc "
        "JOIN information_schema.key_column_usage kcu "
        "  ON kcu.constraint_name=tc.constraint_name AND kcu.table_schema=tc.table_schema "
        "WHERE tc.table_schema='%1' AND tc.table_name='%2' "
        "AND tc.constraint_type='PRIMARY KEY'").arg(schema, table), &pk, nullptr);
    QSet<QString> pkCols;
    for(const QStringList &row : pk.rows)
        pkCols.insert(row.value(0));

    DbResultSet out;
    out.headers << QStringLiteral("Field") << QStringLiteral("Type")
                << QStringLiteral("Null") << QStringLiteral("Key")
                << QStringLiteral("Default") << QStringLiteral("Extra")
                << QStringLiteral("Comment");
    for(const QStringList &row : cols.rows) {
        const QString name = row.value(0);
        const bool isPk = pkCols.contains(name);
        const QString def = row.value(3);
        const bool isIdentity = row.value(4) == QStringLiteral("YES")
            || def.startsWith(QStringLiteral("nextval("));
        QStringList r;
        r << name << row.value(1)
          << (row.value(2) == QStringLiteral("NO") || isPk
                  ? QStringLiteral("NO") : QStringLiteral("YES"))
          << (isPk ? QStringLiteral("PRI") : QString())
          << (def.isNull() ? QStringLiteral("NULL") : def)
          << (isIdentity ? QStringLiteral("auto_increment") : QString())
          << QString();   /* Comment: needs a separate pg_description join,
                            * not worth it until something reads it */
        out.rows << r;
    }
    return out;
}

/* SHOW INDEX shape from pg_index — a LATERAL unnest(indkey) WITH ORDINALITY
 * walks each index's columns in their real (possibly non-declaration) order. */
DbResultSet PostgresConnection::listIndexes(const QString &db, const QString &table)
{
    DbResultSet rs;
    runBuffered(QStringLiteral(
        "SELECT i.relname, ix.indisunique, ix.indisprimary, a.attname, k.ordinality "
        "FROM pg_index ix "
        "JOIN pg_class t ON t.oid = ix.indrelid "
        "JOIN pg_class i ON i.oid = ix.indexrelid "
        "JOIN pg_namespace n ON n.oid = t.relnamespace "
        "JOIN LATERAL unnest(ix.indkey) WITH ORDINALITY AS k(attnum, ordinality) ON true "
        "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = k.attnum "
        "WHERE n.nspname='%1' AND t.relname='%2' "
        "ORDER BY i.relname, k.ordinality").arg(effectiveSchema(db), table), &rs, nullptr);

    DbResultSet out;
    out.headers << QStringLiteral("Table") << QStringLiteral("Non_unique")
                << QStringLiteral("Key_name") << QStringLiteral("Seq_in_index")
                << QStringLiteral("Column_name");
    for(const QStringList &row : rs.rows) {
        const bool isPk = row.value(2) == QStringLiteral("t");
        const bool isUnique = row.value(1) == QStringLiteral("t");
        QStringList r;
        r << table << (isUnique ? QStringLiteral("0") : QStringLiteral("1"))
          << (isPk ? QStringLiteral("PRIMARY") : row.value(0))
          << row.value(4) << row.value(3);
        out.rows << r;
    }
    return out;
}

/* one row per (constraint, column): walks conkey/confkey position-by-
 * position via a second unnest so multi-column FKs pair up the right
 * child/parent columns, not a cross product of them. */
DbResultSet PostgresConnection::listForeignKeys(const QString &db, const QString &table)
{
    DbResultSet rs;
    runBuffered(QStringLiteral(
        "SELECT con.conname, att2.attname, cl.relname, att.attname, "
        "  CASE con.confupdtype WHEN 'a' THEN 'NO ACTION' WHEN 'r' THEN 'RESTRICT' "
        "    WHEN 'c' THEN 'CASCADE' WHEN 'n' THEN 'SET NULL' WHEN 'd' THEN 'SET DEFAULT' "
        "    ELSE 'NO ACTION' END, "
        "  CASE con.confdeltype WHEN 'a' THEN 'NO ACTION' WHEN 'r' THEN 'RESTRICT' "
        "    WHEN 'c' THEN 'CASCADE' WHEN 'n' THEN 'SET NULL' WHEN 'd' THEN 'SET DEFAULT' "
        "    ELSE 'NO ACTION' END "
        "FROM pg_constraint con "
        "JOIN pg_class rel ON rel.oid = con.conrelid "
        "JOIN pg_namespace nsp ON nsp.oid = rel.relnamespace "
        "JOIN pg_class cl ON cl.oid = con.confrelid "
        "JOIN LATERAL unnest(con.conkey) WITH ORDINALITY AS ck(attnum, ord) ON true "
        "JOIN LATERAL unnest(con.confkey) WITH ORDINALITY AS ck2(attnum, ord) "
        "  ON ck2.ord = ck.ord "
        "JOIN pg_attribute att2 ON att2.attrelid = con.conrelid AND att2.attnum = ck.attnum "
        "JOIN pg_attribute att ON att.attrelid = con.confrelid AND att.attnum = ck2.attnum "
        "WHERE con.contype='f' AND nsp.nspname='%1' AND rel.relname='%2' "
        "ORDER BY con.conname, ck.ord").arg(effectiveSchema(db), table), &rs, nullptr);

    DbResultSet out;
    out.headers << QStringLiteral("CONSTRAINT_NAME") << QStringLiteral("COLUMN_NAME")
                << QStringLiteral("REFERENCED_TABLE_NAME")
                << QStringLiteral("REFERENCED_COLUMN_NAME")
                << QStringLiteral("UPDATE_RULE") << QStringLiteral("DELETE_RULE");
    out.rows = rs.rows;
    return out;
}

QStringList PostgresConnection::listTriggers(const QString &db)
{
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral(
           "SELECT t.tgname FROM pg_trigger t "
           "JOIN pg_class c ON c.oid=t.tgrelid "
           "JOIN pg_namespace n ON n.oid=c.relnamespace "
           "WHERE NOT t.tgisinternal AND n.nspname='%1'").arg(effectiveSchema(db)),
           &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(0);
    return out;
}

/* tgtype is a bitmask (see Postgres's own trigger.h): ROW=1 BEFORE=2
 * INSERT=4 DELETE=8 UPDATE=16 TRUNCATE=32 INSTEAD=64. A trigger declared on
 * more than one event (legal in Postgres, e.g. "BEFORE INSERT OR UPDATE")
 * reports whichever of INSERT/DELETE/UPDATE/TRUNCATE is checked first below
 * — same one-event-per-row simplification SHOW TRIGGERS effectively forces
 * on the MySQL side of this canonical shape. */
DbResultSet PostgresConnection::listTableTriggers(const QString &db, const QString &table)
{
    DbResultSet rs;
    runBuffered(QStringLiteral(
        "SELECT t.tgname, t.tgtype FROM pg_trigger t "
        "JOIN pg_class c ON c.oid=t.tgrelid "
        "JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE NOT t.tgisinternal AND n.nspname='%1' AND c.relname='%2'")
            .arg(effectiveSchema(db), table), &rs, nullptr);

    DbResultSet out;
    out.headers << QStringLiteral("Trigger") << QStringLiteral("Timing")
                << QStringLiteral("Event");
    for(const QStringList &row : rs.rows) {
        const int type = row.value(1).toInt();
        const QString timing = (type & 64) ? QStringLiteral("INSTEAD OF")
                              : (type & 2)  ? QStringLiteral("BEFORE")
                                            : QStringLiteral("AFTER");
        const QString event = (type & 4)  ? QStringLiteral("INSERT")
                             : (type & 8)  ? QStringLiteral("DELETE")
                             : (type & 16) ? QStringLiteral("UPDATE")
                                           : QStringLiteral("TRUNCATE");
        out.rows << QStringList{ row.value(0), timing, event };
    }
    return out;
}

DbResultSet PostgresConnection::listRoutines(const QString &db)
{
    DbResultSet rs;
    runBuffered(QStringLiteral(
        "SELECT routine_name, routine_type FROM information_schema.routines "
        "WHERE routine_schema='%1'").arg(effectiveSchema(db)), &rs, nullptr);
    return rs;
}

QStringList PostgresConnection::listEvents(const QString &)
{
    return {};   /* Postgres has no built-in scheduled-event equivalent */
}

QString PostgresConnection::showCreate(const QString &kind, const QString &db,
                                       const QString &name, QString *error)
{
    const QString k = kind.toUpper();
    const QString schema = effectiveSchema(db);

    if(k == QStringLiteral("VIEW")) {
        DbResultSet rs;
        if(!runBuffered(QStringLiteral("SELECT pg_get_viewdef('%1.%2'::regclass, true)")
                             .arg(quoteIdent(schema), quoteIdent(name)), &rs, error)
           || rs.rows.isEmpty())
            return {};
        return QStringLiteral("CREATE VIEW %1.%2 AS\n%3")
            .arg(quoteIdent(schema), quoteIdent(name), rs.rows.first().value(0));
    }
    if(k == QStringLiteral("TRIGGER")) {
        DbResultSet rs;
        if(!runBuffered(QStringLiteral(
               "SELECT pg_get_triggerdef(t.oid) FROM pg_trigger t "
               "JOIN pg_class c ON c.oid=t.tgrelid "
               "JOIN pg_namespace n ON n.oid=c.relnamespace "
               "WHERE NOT t.tgisinternal AND n.nspname='%1' AND t.tgname='%2'")
                   .arg(schema, name), &rs, error) || rs.rows.isEmpty()) {
            if(error && error->isEmpty()) *error = QStringLiteral("trigger not found");
            return {};
        }
        return rs.rows.first().value(0);
    }
    if(k == QStringLiteral("PROCEDURE") || k == QStringLiteral("FUNCTION")) {
        DbResultSet rs;
        /* first match only — an overloaded name (same routine, different
         * arg types) picks whichever the catalog returns first rather than
         * disambiguating, a corner not worth the extra UI this pass */
        if(!runBuffered(QStringLiteral(
               "SELECT pg_get_functiondef(p.oid) FROM pg_proc p "
               "JOIN pg_namespace n ON n.oid=p.pronamespace "
               "WHERE n.nspname='%1' AND p.proname='%2' LIMIT 1")
                   .arg(schema, name), &rs, error) || rs.rows.isEmpty()) {
            if(error && error->isEmpty()) *error = QStringLiteral("routine not found");
            return {};
        }
        return rs.rows.first().value(0);
    }
    if(k == QStringLiteral("TABLE")) {
        /* Postgres has no SHOW CREATE TABLE / pg_get_tabledef() — this is a
         * best-effort reconstruction from catalog metadata (columns + PK),
         * not the byte-for-byte original DDL the way SQLite's showCreate
         * (which just replays sqlite_master's stored text) or MySQL's (a
         * real SHOW CREATE TABLE) are. Check constraints, non-PK unique
         * constraints, and per-column comments aren't reconstructed. */
        const DbResultSet cols = listColumns(schema, name);
        if(cols.rows.isEmpty()) {
            if(error) *error = QStringLiteral("table not found");
            return {};
        }
        QStringList lines;
        QStringList pkCols;
        for(const QStringList &row : cols.rows) {
            QString line = QStringLiteral("  %1 %2").arg(quoteIdent(row.value(0)), row.value(1));
            if(row.value(2) == QStringLiteral("NO"))
                line += QStringLiteral(" NOT NULL");
            if(row.value(4) != QStringLiteral("NULL") && !row.value(4).isEmpty())
                line += QStringLiteral(" DEFAULT %1").arg(row.value(4));
            lines << line;
            if(row.value(3) == QStringLiteral("PRI"))
                pkCols << quoteIdent(row.value(0));
        }
        if(!pkCols.isEmpty())
            lines << QStringLiteral("  PRIMARY KEY (%1)").arg(pkCols.join(QStringLiteral(", ")));
        return QStringLiteral("CREATE TABLE %1.%2 (\n%3\n)")
            .arg(quoteIdent(schema), quoteIdent(name), lines.join(QStringLiteral(",\n")));
    }
    if(error) *error = QStringLiteral("not supported on PostgreSQL");
    return {};
}

QString PostgresConnection::sqlFkChecks(bool enable)
{
    /* session_replication_role disables every trigger (Postgres implements
     * FK enforcement as internal triggers, so this is the closest thing to
     * MySQL's FOREIGN_KEY_CHECKS/SQLite's PRAGMA foreign_keys as a single
     * session-wide toggle) — but setting it needs superuser (or a managed-
     * Postgres equivalent role). A dump/replay run as a lower-privilege
     * user will get a permission-denied error on this statement rather
     * than silently skipping FK enforcement, which is the right failure
     * mode: surfaced, not swallowed. */
    return QStringLiteral("SET session_replication_role = '%1'")
        .arg(enable ? QStringLiteral("origin") : QStringLiteral("replica"));
}

QString PostgresConnection::sqlSetNames(const QString &charset)
{
    static const QHash<QString, QString> kMap = {
        { QStringLiteral("utf8mb4"), QStringLiteral("UTF8") },
        { QStringLiteral("utf8"), QStringLiteral("UTF8") },
        { QStringLiteral("utf16"), QStringLiteral("UTF8") },
        { QStringLiteral("utf16le"), QStringLiteral("UTF8") },
        { QStringLiteral("latin1"), QStringLiteral("LATIN1") },
        { QStringLiteral("ascii"), QStringLiteral("SQL_ASCII") },
        { QStringLiteral("big5"), QStringLiteral("BIG5") },
        { QStringLiteral("gbk"), QStringLiteral("GBK") },
        { QStringLiteral("sjis"), QStringLiteral("SJIS") },
        { QStringLiteral("euckr"), QStringLiteral("EUC_KR") },
        { QStringLiteral("cp1250"), QStringLiteral("WIN1250") },
        { QStringLiteral("cp1251"), QStringLiteral("WIN1251") },
        { QStringLiteral("cp1252"), QStringLiteral("WIN1252") },
    };
    /* best-effort name translation — MySQL's charset list (what feeds this
     * call today, via the CSV/XML import dialogs) and Postgres's encoding
     * names overlap but don't match 1:1; anything not in the table above
     * is passed through uppercased and left to Postgres to accept or
     * reject rather than silently substituting UTF8 for an unknown name */
    return QStringLiteral("SET client_encoding TO '%1'")
        .arg(kMap.value(charset.toLower(), charset.toUpper()));
}

QString PostgresConnection::sqlInsertDefaults(const QString &db, const QString &table)
{
    return QStringLiteral("INSERT INTO %1 DEFAULT VALUES").arg(qualify(db, table));
}

QString PostgresConnection::sqlTruncateTable(const QString &db, const QString &table)
{
    return QStringLiteral("TRUNCATE TABLE %1").arg(qualify(db, table));
}

bool PostgresConnection::supportsLimitOnUpdateDelete()
{
    return false;   /* needs a ctid/PK subquery instead — not supported directly */
}
