#include "SqlDump.h"

#include <QDateTime>
#include <QIODevice>

namespace {

bool query(MYSQL *c, const QByteArray &sql, QString *error)
{
    if(mysql_query(c, sql.constData()) == 0)
        return true;
    if(error)
        *error = QString::fromUtf8(mysql_error(c));
    return false;
}

QStringList baseTables(MYSQL *c, const QString &db, QString *error)
{
    QStringList out;
    const QByteArray sql =
        "SHOW FULL TABLES FROM `" + QByteArray(db.toUtf8()).replace('`', "``")
        + "` WHERE Table_type = 'BASE TABLE'";
    if(!query(c, sql, error))
        return out;
    if(MYSQL_RES *res = mysql_store_result(c)) {
        while(MYSQL_ROW row = mysql_fetch_row(res))
            if(row[0])
                out << QString::fromUtf8(row[0]);
        mysql_free_result(res);
    }
    return out;
}

QString showCreate(MYSQL *c, const QString &db, const QString &table,
                   QString *error)
{
    const QByteArray sql =
        "SHOW CREATE TABLE `" + QByteArray(db.toUtf8()).replace('`', "``")
        + "`.`" + QByteArray(table.toUtf8()).replace('`', "``") + "`";
    if(!query(c, sql, error))
        return {};
    QString ddl;
    if(MYSQL_RES *res = mysql_store_result(c)) {
        if(MYSQL_ROW row = mysql_fetch_row(res))
            ddl = QString::fromUtf8(row[1]);   /* col 1 = "Create Table" */
        mysql_free_result(res);
    }
    return ddl;
}

/* one INSERT tuple: NULL stays NULL, everything else single-quoted + escaped */
QString tuple(MYSQL *c, MYSQL_ROW row, unsigned long *lengths, unsigned n)
{
    QString s = QStringLiteral("(");
    QByteArray esc;
    for(unsigned i = 0; i < n; ++i) {
        if(i)
            s += QLatin1Char(',');
        if(!row[i]) {
            s += QStringLiteral("NULL");
            continue;
        }
        esc.resize(int(lengths[i]) * 2 + 1);
        const unsigned long m = mysql_real_escape_string(
            c, esc.data(), row[i], lengths[i]);
        s += QLatin1Char('\'');
        s += QString::fromUtf8(esc.constData(), int(m));
        s += QLatin1Char('\'');
    }
    s += QLatin1Char(')');
    return s;
}

} // namespace

bool SqlDump::write(MYSQL *conn, const QString &db, const QStringList &tables,
                    const Options &opt, QIODevice *out, QString *error)
{
    if(!conn || !out) {
        if(error)
            *error = QStringLiteral("no connection / output");
        return false;
    }

    QStringList list = tables;
    if(list.isEmpty()) {
        list = baseTables(conn, db, error);
        if(list.isEmpty() && error && !error->isEmpty())
            return false;
    }

    const QByteArray qdb = QByteArray(db.toUtf8()).replace('`', "``");
    auto put = [&](const QString &line) {
        out->write(line.toUtf8());
        out->write("\n");
    };

    put(QStringLiteral("-- OpenYog SQL dump"));
    put(QStringLiteral("-- Database: %1").arg(db));
    put(QStringLiteral("-- Generated: %1")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    put(QStringLiteral("SET FOREIGN_KEY_CHECKS=0;"));
    put(QStringLiteral("SET NAMES utf8mb4;"));
    put(QString());

    for(const QString &t : std::as_const(list)) {
        const QByteArray qt = QByteArray(t.toUtf8()).replace('`', "``");
        put(QStringLiteral("-- ----------------------------"));
        put(QStringLiteral("-- Table: `%1`").arg(t));
        put(QStringLiteral("-- ----------------------------"));

        if(opt.structure) {
            if(opt.addDropTable)
                put(QStringLiteral("DROP TABLE IF EXISTS `%1`;").arg(t));
            const QString ddl = showCreate(conn, db, t, error);
            if(ddl.isEmpty())
                return false;
            put(ddl + QLatin1Char(';'));
            put(QString());
        }

        if(!opt.data)
            continue;

        if(!query(conn, "SELECT * FROM `" + qdb + "`.`" + qt + "`", error))
            return false;
        MYSQL_RES *res = mysql_use_result(conn);
        if(!res) {
            if(error)
                *error = QString::fromUtf8(mysql_error(conn));
            return false;
        }
        const unsigned n = mysql_num_fields(res);
        int inBatch = 0;
        MYSQL_ROW row;
        while((row = mysql_fetch_row(res))) {
            unsigned long *lengths = mysql_fetch_lengths(res);
            if(inBatch == 0)
                out->write(QStringLiteral("INSERT INTO `%1` VALUES\n").arg(t).toUtf8());
            else
                out->write(",\n");
            out->write(tuple(conn, row, lengths, n).toUtf8());
            if(++inBatch >= qMax(1, opt.rowsPerInsert)) {
                out->write(";\n");
                inBatch = 0;
            }
        }
        if(inBatch > 0)
            out->write(";\n");
        mysql_free_result(res);
        put(QString());
    }

    put(QStringLiteral("SET FOREIGN_KEY_CHECKS=1;"));
    return true;
}
