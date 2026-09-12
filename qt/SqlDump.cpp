#include "SqlDump.h"
#include "db/IDbConnection.h"

#include <QDateTime>
#include <QIODevice>
#include <QRegularExpression>

namespace {

QStringList baseTables(IDbConnection *c, const QString &db, QString *error)
{
    DbResultSet rs;
    if(!c->query(QStringLiteral(
           "SHOW FULL TABLES FROM `%1` WHERE Table_type = 'BASE TABLE'")
               .arg(QString(db).replace('`', QStringLiteral("``"))), &rs, error))
        return {};
    QStringList out;
    for(const QStringList &row : rs.rows)
        out << row.value(0);
    return out;
}

/* one INSERT tuple: NULL stays NULL, everything else single-quoted + escaped */
QString tuple(IDbConnection *c, const QVector<QByteArray> &fields,
             const QVector<bool> &isNull)
{
    QString s = QStringLiteral("(");
    for(int i = 0; i < fields.size(); ++i) {
        if(i)
            s += QLatin1Char(',');
        if(isNull[i]) {
            s += QStringLiteral("NULL");
            continue;
        }
        s += QLatin1Char('\'');
        s += QString::fromUtf8(c->escape(fields[i]));
        s += QLatin1Char('\'');
    }
    s += QLatin1Char(')');
    return s;
}

} // namespace

bool SqlDump::write(IDbConnection *conn, const QString &db, const QStringList &tables,
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
            const QString ddl = conn->showCreate(QStringLiteral("TABLE"), db, t, error);
            if(ddl.isEmpty())
                return false;
            put(ddl + QLatin1Char(';'));
            put(QString());
        }

        if(!opt.data)
            continue;

        int inBatch = 0;
        bool streamOk = conn->streamQuery(
            QStringLiteral("SELECT * FROM `") + QString::fromUtf8(qdb) + "`.`"
                + QString::fromUtf8(qt) + "`",
            error, nullptr,
            [&](const QVector<QByteArray> &fields, const QVector<bool> &isNull) {
                if(inBatch == 0)
                    out->write(QStringLiteral("INSERT INTO `%1` VALUES\n").arg(t).toUtf8());
                else
                    out->write(",\n");
                out->write(tuple(conn, fields, isNull).toUtf8());
                if(++inBatch >= qMax(1, opt.rowsPerInsert)) {
                    out->write(";\n");
                    inBatch = 0;
                }
                return true;
            });
        if(!streamOk)
            return false;
        if(inBatch > 0)
            out->write(";\n");
        put(QString());
    }

    put(QStringLiteral("SET FOREIGN_KEY_CHECKS=1;"));
    return true;
}

bool SqlDump::forEachStatement(
    IDbConnection *conn, const QString &db, const QStringList &tables,
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
            const QString ddl = conn->showCreate(QStringLiteral("TABLE"), db, t, error);
            if(ddl.isEmpty() || !exec(ddl))
                return false;
        }
        if(!opt.data)
            continue;

        QString batch;
        int inBatch = 0;
        bool execFailed = false;
        const auto flush = [&] {
            if(inBatch == 0)
                return true;
            const bool ok = exec(batch);
            batch.clear();
            inBatch = 0;
            return ok;
        };
        bool streamOk = conn->streamQuery(
            QStringLiteral("SELECT * FROM `") + QString::fromUtf8(qdb) + "`.`"
                + QString::fromUtf8(qt) + "`",
            error, nullptr,
            [&](const QVector<QByteArray> &fields, const QVector<bool> &isNull) {
                if(inBatch == 0)
                    batch = QStringLiteral("INSERT INTO `%1` VALUES\n").arg(t);
                else
                    batch += QStringLiteral(",\n");
                batch += tuple(conn, fields, isNull);
                if(++inBatch >= qMax(1, opt.rowsPerInsert) && !flush()) {
                    execFailed = true;
                    return false;
                }
                return true;
            });
        if(!streamOk || execFailed)
            return false;
        if(!flush())
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
            DbResultSet rs;
            if(conn->query(sql, &rs, error))
                for(const QStringList &row : rs.rows)
                    out << row.value(col);
            return out;
        };
        const auto one = [&](const QString &sql, int col) {
            DbResultSet rs;
            if(conn->query(sql, &rs, error) && !rs.rows.isEmpty())
                return rs.rows.first().value(col);
            return QString();
        };
        const QString dq = QString(db).replace('`', QStringLiteral("``"));

        for(const QString &v : names(
                QStringLiteral("SHOW FULL TABLES FROM `%1` WHERE Table_type='VIEW'").arg(dq), 0)) {
            const QString ddl = clean(one(
                QStringLiteral("SHOW CREATE VIEW `%1`.`%2`").arg(dq, v), 1));
            if(!ddl.isEmpty() && !exec(ddl))
                return false;
        }
        {
            DbResultSet rs;
            if(conn->query(QStringLiteral("SELECT ROUTINE_NAME, ROUTINE_TYPE FROM "
                         "information_schema.ROUTINES WHERE ROUTINE_SCHEMA='%1'")
                         .arg(dq), &rs, error)) {
                for(const QStringList &row : rs.rows) {
                    const QString kw = row.value(1) == QStringLiteral("PROCEDURE")
                        ? QStringLiteral("PROCEDURE") : QStringLiteral("FUNCTION");
                    const QString ddl = clean(one(
                        QStringLiteral("SHOW CREATE %1 `%2`.`%3`").arg(kw, dq, row.value(0)), 2));
                    if(!ddl.isEmpty() && !exec(ddl))
                        return false;
                }
            }
        }
        {
            DbResultSet rs;
            if(conn->query(QStringLiteral("SHOW TRIGGERS FROM `%1`").arg(dq), &rs, error)) {
                QStringList trg;
                for(const QStringList &row : rs.rows)
                    trg << QStringLiteral("CREATE TRIGGER `%1` %2 %3 ON `%4` "
                                          "FOR EACH ROW %5")
                        .arg(row.value(0), row.value(4), row.value(1),
                             row.value(2), row.value(3));
                for(const QString &s : std::as_const(trg))
                    if(!exec(s))
                        return false;
            }
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
