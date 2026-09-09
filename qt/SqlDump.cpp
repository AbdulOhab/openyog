#include "SqlDump.h"

#include <QDateTime>
#include <QIODevice>
#include <QRegularExpression>

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

bool SqlDump::forEachStatement(
    MYSQL *conn, const QString &db, const QStringList &tables,
    const Options &opt, const std::function<bool(const QString &)> &exec,
    QString *error)
{
    if(!conn) {
        if(error) *error = QStringLiteral("no connection");
        return false;
    }
    QStringList list = tables;
    if(list.isEmpty()) {
        list = baseTables(conn, db, error);
        if(list.isEmpty() && error && !error->isEmpty())
            return false;
    }
    const QByteArray qdb = QByteArray(db.toUtf8()).replace('`', "``");

    if(!exec(QStringLiteral("SET FOREIGN_KEY_CHECKS=0")))
        return false;

    for(const QString &t : std::as_const(list)) {
        const QByteArray qt = QByteArray(t.toUtf8()).replace('`', "``");
        if(opt.structure) {
            if(opt.addDropTable
               && !exec(QStringLiteral("DROP TABLE IF EXISTS `%1`").arg(t)))
                return false;
            const QString ddl = showCreate(conn, db, t, error);
            if(ddl.isEmpty() || !exec(ddl))
                return false;
        }
        if(!opt.data)
            continue;

        if(!query(conn, "SELECT * FROM `" + qdb + "`.`" + qt + "`", error))
            return false;
        MYSQL_RES *res = mysql_use_result(conn);
        if(!res) {
            if(error) *error = QString::fromUtf8(mysql_error(conn));
            return false;
        }
        const unsigned n = mysql_num_fields(res);
        QString batch;
        int inBatch = 0;
        const auto flush = [&] {
            if(inBatch == 0)
                return true;
            const bool ok = exec(batch);
            batch.clear();
            inBatch = 0;
            return ok;
        };
        MYSQL_ROW row;
        bool ok = true;
        while(ok && (row = mysql_fetch_row(res))) {
            unsigned long *lengths = mysql_fetch_lengths(res);
            if(inBatch == 0)
                batch = QStringLiteral("INSERT INTO `%1` VALUES\n").arg(t);
            else
                batch += QStringLiteral(",\n");
            batch += tuple(conn, row, lengths, n);
            if(++inBatch >= qMax(1, opt.rowsPerInsert))
                ok = flush();
        }
        if(ok)
            ok = flush();
        mysql_free_result(res);
        if(!ok)
            return false;
    }

    if(opt.routines) {
        static const QRegularExpression kDefiner(
            QStringLiteral("DEFINER=`[^`]*`@`[^`]*` "));
        /* strip DEFINER and the source-db qualifier — the caller has USE'd
         * the target db, so unqualified names land there */
        const auto clean = [&](QString s) {
            return s.remove(kDefiner)
                    .replace(QStringLiteral("`%1`.").arg(db), QString());
        };
        const auto names = [&](const QString &sql, int col) {
            QStringList out;
            if(query(conn, sql.toUtf8(), error)) {
                if(MYSQL_RES *r = mysql_store_result(conn)) {
                    while(MYSQL_ROW row = mysql_fetch_row(r))
                        if(row[col]) out << QString::fromUtf8(row[col]);
                    mysql_free_result(r);
                }
            }
            return out;
        };
        const auto one = [&](const QString &sql, int col) {
            QString v;
            if(query(conn, sql.toUtf8(), error)) {
                if(MYSQL_RES *r = mysql_store_result(conn)) {
                    if(MYSQL_ROW row = mysql_fetch_row(r))
                        v = QString::fromUtf8(row[col] ? row[col] : "");
                    mysql_free_result(r);
                }
            }
            return v;
        };
        const QString dq = QString(db).replace('`', QStringLiteral("``"));

        for(const QString &v : names(
                QStringLiteral("SHOW FULL TABLES FROM `%1` WHERE Table_type='VIEW'").arg(dq), 0)) {
            const QString ddl = clean(one(
                QStringLiteral("SHOW CREATE VIEW `%1`.`%2`").arg(dq, v), 1));
            if(!ddl.isEmpty() && !exec(ddl))
                return false;
        }
        if(query(conn, QStringLiteral("SELECT ROUTINE_NAME, ROUTINE_TYPE FROM "
                     "information_schema.ROUTINES WHERE ROUTINE_SCHEMA='%1'")
                     .arg(dq).toUtf8(), error)) {
            QList<QPair<QString, QString>> rs;
            if(MYSQL_RES *r = mysql_store_result(conn)) {
                while(MYSQL_ROW row = mysql_fetch_row(r))
                    if(row[0] && row[1])
                        rs << qMakePair(QString::fromUtf8(row[0]),
                                        QString::fromUtf8(row[1]));
                mysql_free_result(r);
            }
            for(const auto &rt : std::as_const(rs)) {
                const QString kw = rt.second == QStringLiteral("PROCEDURE")
                    ? QStringLiteral("PROCEDURE") : QStringLiteral("FUNCTION");
                const QString ddl = clean(one(
                    QStringLiteral("SHOW CREATE %1 `%2`.`%3`").arg(kw, dq, rt.first), 2));
                if(!ddl.isEmpty() && !exec(ddl))
                    return false;
            }
        }
        if(query(conn, QStringLiteral("SHOW TRIGGERS FROM `%1`").arg(dq).toUtf8(), error)) {
            QStringList trg;
            if(MYSQL_RES *r = mysql_store_result(conn)) {
                while(MYSQL_ROW row = mysql_fetch_row(r)) {
                    if(!row[0]) continue;
                    trg << QStringLiteral("CREATE TRIGGER `%1` %2 %3 ON `%4` "
                                          "FOR EACH ROW %5")
                        .arg(QString::fromUtf8(row[0]),
                             QString::fromUtf8(row[4] ? row[4] : ""),
                             QString::fromUtf8(row[1] ? row[1] : ""),
                             QString::fromUtf8(row[2] ? row[2] : ""),
                             QString::fromUtf8(row[3] ? row[3] : ""));
                }
                mysql_free_result(r);
            }
            for(const QString &s : std::as_const(trg))
                if(!exec(s))
                    return false;
        }
        for(const QString &e : names(
                QStringLiteral("SHOW EVENTS FROM `%1`").arg(dq), 1)) {
            const QString ddl = clean(one(
                QStringLiteral("SHOW CREATE EVENT `%1`.`%2`").arg(dq, e), 3));
            if(!ddl.isEmpty() && !exec(ddl))
                return false;
        }
    }

    return exec(QStringLiteral("SET FOREIGN_KEY_CHECKS=1"));
}
