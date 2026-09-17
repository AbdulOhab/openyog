#include "SqlDump.h"
#include "db/IDbConnection.h"

#include <QDateTime>
#include <QIODevice>
#include <QRegularExpression>

namespace {

QStringList baseTables(IDbConnection *c, const QString &db, QString *error)
{
    const QStringList out = c->listTables(db, QStringLiteral("BASE TABLE"));
    /* an empty table list is normal for an empty db — but a db we cannot see
     * at all is a hard error (upstream BUG-1: never report success and leave
     * a bogus empty target behind). Only meaningful when a specific db name
     * was actually requested: an empty `db` is SQLite's normal "no database
     * concept, the file is main" convention, not "no db selected" — and
     * listDatabases() for SQLite returns "main", never "", so this check
     * would otherwise misfire "does not exist" on every empty-but-valid
     * SQLite file. */
    if(out.isEmpty() && !db.isEmpty() && !c->listDatabases().contains(db) && error)
        *error = QStringLiteral("database '%1' does not exist on this connection").arg(db);
    return out;
}

/* one INSERT tuple: NULL stays NULL, everything else single-quoted + escaped */
QString tuple(IDbConnection *c, const QVector<QByteArray> &fields, const QVector<bool> &isNull)
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

    auto put = [&](const QString &line) {
        out->write(line.toUtf8());
        out->write("\n");
    };

    put(QStringLiteral("-- OpenYog SQL dump"));
    put(QStringLiteral("-- Database: %1").arg(db));
    put(QStringLiteral("-- Generated: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    const QString fkOff = conn->sqlFkChecks(false);
    if(!fkOff.isEmpty())
        put(fkOff + QLatin1Char(';'));
    const QString names = conn->sqlSetNames(QStringLiteral("utf8mb4"));
    if(!names.isEmpty())
        put(names + QLatin1Char(';'));
    put(QString());

    for(const QString &t : std::as_const(list)) {
        put(QStringLiteral("-- ----------------------------"));
        put(QStringLiteral("-- Table: %1").arg(conn->quoteIdent(t)));
        put(QStringLiteral("-- ----------------------------"));

        if(opt.structure) {
            if(opt.addDropTable)
                put(QStringLiteral("DROP TABLE IF EXISTS %1;").arg(conn->quoteIdent(t)));
            const QString ddl = conn->showCreate(QStringLiteral("TABLE"), db, t, error);
            if(ddl.isEmpty())
                return false;
            put(ddl + QLatin1Char(';'));
            put(QString());
        }

        if(!opt.data)
            continue;

        const QString select = QStringLiteral("SELECT * FROM %1").arg(conn->qualify(db, t));
        int inBatch = 0;
        bool streamOk =
            conn->streamQuery(select, error, nullptr,
                              [&](const QVector<QByteArray> &fields, const QVector<bool> &isNull) {
                                  if(inBatch == 0)
                                      out->write(QStringLiteral("INSERT INTO %1 VALUES\n")
                                                     .arg(conn->quoteIdent(t))
                                                     .toUtf8());
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

    const QString fkOn = conn->sqlFkChecks(true);
    if(!fkOn.isEmpty())
        put(fkOn + QLatin1Char(';'));
    return true;
}

bool SqlDump::forEachStatement(IDbConnection *conn, const QString &db, const QStringList &tables,
                               const Options &opt, const std::function<bool(const QString &)> &exec,
                               QString *error)
{
    if(!conn) {
        if(error)
            *error = QStringLiteral("no connection");
        return false;
    }
    QStringList list = tables;
    if(list.isEmpty()) {
        list = baseTables(conn, db, error);
        if(list.isEmpty() && error && !error->isEmpty())
            return false;
    }

    if(!exec(conn->sqlFkChecks(false)))
        return false;

    for(const QString &t : std::as_const(list)) {
        if(opt.structure) {
            if(opt.addDropTable &&
               !exec(QStringLiteral("DROP TABLE IF EXISTS %1").arg(conn->quoteIdent(t))))
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
        const QString select = QStringLiteral("SELECT * FROM %1").arg(conn->qualify(db, t));
        bool streamOk = conn->streamQuery(
            select, error, nullptr,
            [&](const QVector<QByteArray> &fields, const QVector<bool> &isNull) {
                if(inBatch == 0)
                    batch = QStringLiteral("INSERT INTO %1 VALUES\n").arg(conn->quoteIdent(t));
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
        /* strip DEFINER=`u`@`h` (MySQL) and the source-db qualifier — the
         * caller has USEd the target db, so unqualified names land there */
        static const QRegularExpression kDefiner(QStringLiteral("DEFINER=`[^`]*`@`[^`]*` "));
        const auto clean = [&](QString s) {
            return s.remove(kDefiner).replace(QStringLiteral("`%1`.").arg(db), QString());
        };
        /* showCreate failure on an unsupported kind yields an empty DDL —
         * skipped (e.g. SQLite: routines/events don't exist, and its
         * triggers already replay as complete CREATE TRIGGER statements) */
        const auto ddlFor = [&](const QString &kind, const QString &name) {
            QString err;
            return clean(conn->showCreate(kind, db, name, &err));
        };

        for(const QString &v : conn->listTables(db, QStringLiteral("VIEW"))) {
            const QString ddl = ddlFor(QStringLiteral("VIEW"), v);
            if(!ddl.isEmpty() && !exec(ddl))
                return false;
        }
        for(const QStringList &row : conn->listRoutines(db).rows) {
            const QString kw = row.value(1) == QStringLiteral("PROCEDURE")
                                   ? QStringLiteral("PROCEDURE")
                                   : QStringLiteral("FUNCTION");
            const QString ddl = ddlFor(kw, row.value(0));
            if(!ddl.isEmpty() && !exec(ddl))
                return false;
        }
        for(const QString &t : conn->listTriggers(db)) {
            const QString ddl = ddlFor(QStringLiteral("TRIGGER"), t);
            if(!ddl.isEmpty() && !exec(ddl))
                return false;
        }
        for(const QString &e : conn->listEvents(db)) {
            const QString ddl = ddlFor(QStringLiteral("EVENT"), e);
            if(!ddl.isEmpty() && !exec(ddl))
                return false;
        }
    }

    const QString fkOn = conn->sqlFkChecks(true);
    return fkOn.isEmpty() || exec(fkOn);
}
