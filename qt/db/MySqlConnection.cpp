#include "MySqlConnection.h"

MySqlConnection::MySqlConnection(MYSQL *conn, const QString &host, int port,
                                 const QString &user, const QString &password,
                                 bool owns)
    : m_conn(conn), m_owns(owns), m_host(host), m_user(user),
      m_password(password), m_port(port)
{
}

MySqlConnection::~MySqlConnection()
{
    if(m_conn && m_owns)
        mysql_close(m_conn);
}

void MySqlConnection::cancel()
{
    if(!m_conn)
        return;
    const unsigned long id = mysql_thread_id(m_conn);   /* safe to read concurrently */
    /* the busy connection can't process KILL QUERY itself — open a
     * throwaway one just to send it, matching mysql's own "mysqladmin
     * kill"/Ctrl+C behavior */
    MYSQL *aux = mysql_init(nullptr);
    if(!aux)
        return;
    if(mysql_real_connect(aux, m_host.toUtf8().constData(), m_user.toUtf8().constData(),
                          m_password.toUtf8().constData(), nullptr, m_port, nullptr, 0)) {
        const QByteArray sql = QStringLiteral("KILL QUERY %1").arg(id).toUtf8();
        mysql_query(aux, sql.constData());
    }
    mysql_close(aux);
}

bool MySqlConnection::runBuffered(const QString &sql, DbResultSet *out, QString *error)
{
    const QByteArray utf8 = sql.toUtf8();
    if(mysql_query(m_conn, utf8.constData()) != 0) {
        if(error) *error = QString::fromUtf8(mysql_error(m_conn));
        return false;
    }
    MYSQL_RES *res = mysql_store_result(m_conn);
    if(!res) {
        if(mysql_field_count(m_conn) != 0) {
            if(error) *error = QString::fromUtf8(mysql_error(m_conn));
            return false;
        }
        return true;   /* statement had no result set (DDL/DML) */
    }
    if(out) {
        const unsigned int n = mysql_num_fields(res);
        MYSQL_FIELD *fields = mysql_fetch_fields(res);
        for(unsigned int i = 0; i < n; ++i)
            out->headers << QString::fromUtf8(fields[i].name);
        while(MYSQL_ROW row = mysql_fetch_row(res)) {
            QStringList r;
            for(unsigned int i = 0; i < n; ++i)
                r << (row[i] ? QString::fromUtf8(row[i]) : QStringLiteral("NULL"));
            out->rows << r;
        }
    }
    mysql_free_result(res);
    return true;
}

bool MySqlConnection::query(const QString &sql, DbResultSet *result, QString *message)
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

bool MySqlConnection::streamQuery(
    const QString &sql, QString *error,
    const std::function<void(const QStringList &headers)> &onHeaders,
    const std::function<bool(const QVector<QByteArray> &fields,
                              const QVector<bool> &isNull)> &onRow)
{
    const QByteArray utf8 = sql.toUtf8();
    if(mysql_query(m_conn, utf8.constData()) != 0) {
        if(error) *error = QString::fromUtf8(mysql_error(m_conn));
        return false;
    }
    MYSQL_RES *res = mysql_use_result(m_conn);
    if(!res) {
        if(error) *error = QString::fromUtf8(mysql_error(m_conn));
        return false;
    }
    const unsigned int n = mysql_num_fields(res);
    if(onHeaders) {
        QStringList headers;
        MYSQL_FIELD *fields = mysql_fetch_fields(res);
        for(unsigned int i = 0; i < n; ++i)
            headers << QString::fromUtf8(fields[i].name);
        onHeaders(headers);
    }
    MYSQL_ROW row;
    while((row = mysql_fetch_row(res))) {
        unsigned long *lengths = mysql_fetch_lengths(res);
        QVector<QByteArray> fields(n);
        QVector<bool> isNull(n);
        for(unsigned int i = 0; i < n; ++i) {
            isNull[i] = !row[i];
            if(row[i])
                fields[i] = QByteArray(row[i], int(lengths[i]));
        }
        if(onRow && !onRow(fields, isNull))
            break;
    }
    mysql_free_result(res);
    return true;
}

QByteArray MySqlConnection::escape(const QByteArray &raw)
{
    QByteArray esc(raw.size() * 2 + 1, '\0');
    const unsigned long n = mysql_real_escape_string(m_conn, esc.data(), raw.constData(),
                                                      (unsigned long)raw.size());
    esc.resize(int(n));
    return esc;
}

QString MySqlConnection::quoteIdent(const QString &ident)
{
    return QLatin1Char('`') + QString(ident).replace(QLatin1Char('`'), QStringLiteral("``"))
         + QLatin1Char('`');
}

QString MySqlConnection::lastError() { return QString::fromUtf8(mysql_error(m_conn)); }
qint64  MySqlConnection::affectedRows() { return (qint64)mysql_affected_rows(m_conn); }
QString MySqlConnection::serverInfo() { return QString::fromUtf8(mysql_get_server_info(m_conn)); }
QString MySqlConnection::info()
{
    const char *i = mysql_info(m_conn);
    return i ? QString::fromUtf8(i) : QString();
}

QStringList MySqlConnection::listDatabases()
{
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral("SHOW DATABASES"), &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(0);
    return out;
}

QStringList MySqlConnection::listTables(const QString &db, const QString &typeFilter)
{
    QString sql = QStringLiteral("SHOW FULL TABLES FROM %1").arg(quoteIdent(db));
    if(!typeFilter.isEmpty())
        sql += QStringLiteral(" WHERE Table_type='%1'").arg(typeFilter);
    DbResultSet rs;
    QStringList out;
    if(runBuffered(sql, &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(0);
    return out;
}

DbResultSet MySqlConnection::listColumns(const QString &db, const QString &table)
{
    /* SHOW FULL COLUMNS: Field(0) Type(1) Collation(2) Null(3) Key(4)
     * Default(5) Extra(6) Privileges(7) Comment(8) — reshaped to the
     * canonical Field/Type/Null/Key/Default/Extra/Comment order (drops
     * Collation/Privileges, moves Comment to the end at index 6). */
    DbResultSet full;
    runBuffered(QStringLiteral("SHOW FULL COLUMNS FROM %1.%2")
                    .arg(quoteIdent(db), quoteIdent(table)), &full, nullptr);
    DbResultSet rs;
    rs.headers << QStringLiteral("Field") << QStringLiteral("Type")
               << QStringLiteral("Null") << QStringLiteral("Key")
               << QStringLiteral("Default") << QStringLiteral("Extra")
               << QStringLiteral("Comment");
    for(const QStringList &row : full.rows)
        rs.rows << (QStringList()
            << row.value(0) << row.value(1) << row.value(3) << row.value(4)
            << row.value(5) << row.value(6) << row.value(8));
    return rs;
}

/* one information_schema query for every distinct column name in `db`,
 * instead of the default's SHOW FULL COLUMNS round trip per table */
QStringList MySqlConnection::listAllColumnNames(const QString &db)
{
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral(
           "SELECT DISTINCT column_name FROM information_schema.columns "
           "WHERE table_schema='%1' ORDER BY column_name")
               .arg(db), &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(0);
    return out;
}

DbResultSet MySqlConnection::listIndexes(const QString &db, const QString &table)
{
    DbResultSet rs;
    runBuffered(QStringLiteral("SHOW INDEX FROM %1.%2")
                    .arg(quoteIdent(db), quoteIdent(table)), &rs, nullptr);
    return rs;
}

DbResultSet MySqlConnection::listForeignKeys(const QString &db, const QString &table)
{
    DbResultSet rs;
    runBuffered(QStringLiteral(
        "SELECT k.CONSTRAINT_NAME, k.COLUMN_NAME, k.REFERENCED_TABLE_NAME, "
        "k.REFERENCED_COLUMN_NAME, r.UPDATE_RULE, r.DELETE_RULE "
        "FROM information_schema.KEY_COLUMN_USAGE k "
        "JOIN information_schema.REFERENTIAL_CONSTRAINTS r "
        "  ON r.CONSTRAINT_SCHEMA=k.CONSTRAINT_SCHEMA "
        " AND r.CONSTRAINT_NAME=k.CONSTRAINT_NAME "
        "WHERE k.TABLE_SCHEMA='%1' AND k.TABLE_NAME='%2' "
        "  AND k.REFERENCED_TABLE_NAME IS NOT NULL "
        "ORDER BY k.CONSTRAINT_NAME, k.ORDINAL_POSITION")
            .arg(QString(db).replace('\'', QStringLiteral("''")),
                 QString(table).replace('\'', QStringLiteral("''"))), &rs, nullptr);
    return rs;
}

QStringList MySqlConnection::listTriggers(const QString &db)
{
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral("SHOW TRIGGERS FROM %1").arg(quoteIdent(db)), &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(0);
    return out;
}

DbResultSet MySqlConnection::listTableTriggers(const QString &db, const QString &table)
{
    DbResultSet rs;
    /* canonical shape: Trigger(0) Timing(1) Event(2) — SHOW TRIGGERS keeps
     * Timing in col 4 and Event in col 1 */
    runBuffered(QStringLiteral("SHOW TRIGGERS FROM %1 WHERE `Table`='%2'")
                    .arg(quoteIdent(db),
                         QString(table).replace('\'', QStringLiteral("''"))),
                &rs, nullptr);
    DbResultSet out;
    out.headers << QStringLiteral("Trigger") << QStringLiteral("Timing")
                << QStringLiteral("Event");
    for(const QStringList &row : rs.rows)
        out.rows << QStringList{ row.value(0), row.value(4), row.value(1) };
    return out;
}

DbResultSet MySqlConnection::listRoutines(const QString &db)
{
    DbResultSet rs;
    runBuffered(QStringLiteral(
        "SELECT ROUTINE_NAME, ROUTINE_TYPE FROM information_schema.ROUTINES "
        "WHERE ROUTINE_SCHEMA='%1'")
            .arg(QString(db).replace('\'', QStringLiteral("''"))), &rs, nullptr);
    return rs;
}

QStringList MySqlConnection::listEvents(const QString &db)
{
    DbResultSet rs;
    QStringList out;
    if(runBuffered(QStringLiteral("SHOW EVENTS FROM %1").arg(quoteIdent(db)), &rs, nullptr))
        for(const QStringList &row : rs.rows)
            out << row.value(1);   /* col 1 = Name */
    return out;
}

QString MySqlConnection::showCreate(const QString &kind, const QString &db,
                                    const QString &name, QString *error)
{
    DbResultSet rs;
    if(!runBuffered(QStringLiteral("SHOW CREATE %1 %2.%3")
                         .arg(kind, quoteIdent(db), quoteIdent(name)), &rs, error))
        return {};
    if(rs.rows.isEmpty())
        return {};
    /* column holding the DDL text varies by object kind (matches the
     * convention in qt/SchemaSql.h's showCreateColumn, plus TABLE here) */
    int col = 2;   /* PROCEDURE / FUNCTION / TRIGGER */
    if(kind == QStringLiteral("TABLE") || kind == QStringLiteral("VIEW"))
        col = 1;
    else if(kind == QStringLiteral("EVENT"))
        col = 3;
    return rs.rows.first().value(col);
}

QString MySqlConnection::sqlFkChecks(bool enable)
{
    return QStringLiteral("SET FOREIGN_KEY_CHECKS=%1").arg(enable ? 1 : 0);
}

QString MySqlConnection::sqlSetNames(const QString &charset)
{
    return QStringLiteral("SET NAMES %1").arg(charset);
}

QString MySqlConnection::sqlInsertDefaults(const QString &db, const QString &table)
{
    return QStringLiteral("INSERT INTO %1 () VALUES ()")
        .arg(db.isEmpty() ? quoteIdent(table)
                          : quoteIdent(db) + QLatin1Char('.') + quoteIdent(table));
}

QString MySqlConnection::sqlTruncateTable(const QString &db, const QString &table)
{
    return QStringLiteral("TRUNCATE TABLE %1")
        .arg(db.isEmpty() ? quoteIdent(table)
                          : quoteIdent(db) + QLatin1Char('.') + quoteIdent(table));
}

bool MySqlConnection::supportsLimitOnUpdateDelete()
{
    return true;
}
