/* OpenYog — Data Sync's comparison and script; see DataSync.h. */
#include "DataSync.h"
#include "SqlDump.h"
#include "db/IDbConnection.h"

#include <QCryptographicHash>
#include <QHash>
#include <QSet>
#include <QTemporaryFile>

#include <algorithm>
#include <functional>

namespace DataSync {

QStringList commonTables(IDbConnection *source, const QString &sourceDb, IDbConnection *target,
                         const QString &targetDb)
{
    const QStringList targetTables = target->listTables(targetDb, QStringLiteral("BASE TABLE"));
    const QSet<QString> inTarget(targetTables.cbegin(), targetTables.cend());
    QStringList common;
    for(const QString &t : source->listTables(sourceDb, QStringLiteral("BASE TABLE")))
        if(inTarget.contains(t))
            common << t;
    const QSet<QString> shared(common.cbegin(), common.cend());

    /* listAllForeignKeys: table, constraint, column, referenced table, … */
    QHash<QString, QStringList> parents;
    for(const QStringList &row : target->listAllForeignKeys(targetDb).rows)
        if(row.value(0) != row.value(3) && shared.contains(row.value(0)) &&
           shared.contains(row.value(3)) && !parents[row.value(0)].contains(row.value(3)))
            parents[row.value(0)] << row.value(3);

    /* each table after its parents; a cycle is cut where the walk meets a
     * table it is still inside */
    QStringList ordered;
    QSet<QString> done, onPath;
    const std::function<void(const QString &)> visit = [&](const QString &t) {
        if(done.contains(t) || onPath.contains(t))
            return;
        onPath.insert(t);
        for(const QString &p : parents.value(t))
            visit(p);
        onPath.remove(t);
        done.insert(t);
        ordered << t;
    };
    for(const QString &t : std::as_const(common))
        visit(t);
    return ordered;
}

namespace {

void appendSized(QByteArray &out, const QByteArray &value)
{
    const quint32 n = quint32(value.size());
    out.append(reinterpret_cast<const char *>(&n), sizeof n);
    out.append(value);
}

/* the key columns' values as one byte string, and back */
QByteArray packKey(const QVector<QByteArray> &fields, int keyColumns)
{
    QByteArray k;
    for(int i = 0; i < keyColumns; ++i)
        appendSized(k, fields[i]);
    return k;
}

QVector<QByteArray> unpackKey(const QByteArray &k)
{
    QVector<QByteArray> fields;
    for(qsizetype at = 0; at + qsizetype(sizeof(quint32)) <= k.size();) {
        quint32 n;
        memcpy(&n, k.constData() + at, sizeof n);
        at += sizeof n;
        fields << k.mid(at, n);
        at += n;
    }
    return fields;
}

/* the other columns' values, NULL told apart from '' */
QByteArray digest(const QVector<QByteArray> &fields, const QVector<bool> &isNull, int keyColumns)
{
    QCryptographicHash h(QCryptographicHash::Md5);
    for(int i = keyColumns; i < fields.size(); ++i) {
        QByteArray part(isNull[i] ? "\0" : "\1", 1);
        appendSized(part, fields[i]);
        h.addData(part);
    }
    return h.result();
}

} // namespace

TableResult compareTable(IDbConnection *source, const QString &sourceDb, IDbConnection *target,
                         const QString &targetDb, const QString &table, const Emit &onStatement,
                         int rowsPerInsert, const std::function<bool()> &cancelled)
{
    TableResult r;
    r.table = table;
    QStringList sourceCols, targetCols, sourceKey, targetKey;
    for(const QStringList &row : source->listColumns(sourceDb, table).rows) {
        sourceCols << row.value(0);
        if(row.value(3) == QStringLiteral("PRI"))
            sourceKey << row.value(0);
    }
    for(const QStringList &row : target->listColumns(targetDb, table).rows) {
        targetCols << row.value(0);
        if(row.value(3) == QStringLiteral("PRI"))
            targetKey << row.value(0);
    }
    for(const QString &c : std::as_const(sourceCols))
        if(!targetCols.contains(c))
            r.ignored << c;
    for(const QString &c : std::as_const(targetCols))
        if(!sourceCols.contains(c))
            r.ignored << c;
    if(sourceKey.isEmpty()) {
        r.skipped = QStringLiteral("no primary key to match rows by");
        return r;
    }
    if(QSet<QString>(sourceKey.cbegin(), sourceKey.cend()) !=
       QSet<QString>(targetKey.cbegin(), targetKey.cend())) {
        r.skipped = QStringLiteral("the two tables' primary keys differ");
        return r;
    }
    r.key = sourceKey;
    r.columns = sourceKey;
    for(const QString &c : std::as_const(sourceCols))
        if(!sourceKey.contains(c) && targetCols.contains(c))
            r.columns << c;
    const int keyColumns = int(r.key.size());

    const QVector<bool> binary = SqlDump::binaryColumns(source, sourceDb, table);
    QVector<bool> hex;
    for(const QString &c : std::as_const(r.columns))
        hex << binary.value(sourceCols.indexOf(c));
    const auto select = [&](IDbConnection *c, const QString &db) {
        QStringList cols;
        for(const QString &col : std::as_const(r.columns))
            cols << c->quoteIdent(col);
        return QStringLiteral("SELECT %1 FROM %2")
            .arg(cols.join(QStringLiteral(", ")), c->qualify(db, table));
    };

    /* the target's rows: key → digest of the rest */
    QHash<QByteArray, QByteArray> targetRows;
    bool stopped = false;
    QString error;
    if(!target->streamQuery(select(target, targetDb), &error, nullptr,
                            [&](const QVector<QByteArray> &f, const QVector<bool> &n) {
                                if(cancelled && cancelled()) {
                                    stopped = true;
                                    return false;
                                }
                                targetRows.insert(packKey(f, keyColumns), digest(f, n, keyColumns));
                                return true;
                            })) {
        r.skipped = error;
        return r;
    }
    if(stopped) {
        r.skipped = QStringLiteral("cancelled");
        return r;
    }

    /* the statements, in the target's dialect */
    const QByteArray qualified = target->qualify(targetDb, table).toUtf8();
    QByteArray columnList;
    for(int i = 0; i < r.columns.size(); ++i)
        columnList += (i ? ", " : "") + target->quoteIdent(r.columns[i]).toUtf8();
    const auto literal = [&](const QVector<QByteArray> &f, const QVector<bool> &n, int i) {
        return SqlDump::literal(target, f[i], n.value(i), hex.value(i));
    };
    const auto where = [&](const QVector<QByteArray> &f) {
        QByteArray w;
        for(int i = 0; i < keyColumns; ++i)
            w += (i ? " AND " : "") + target->quoteIdent(r.key[i]).toUtf8() + " = " +
                 literal(f, {}, i);
        return w;
    };
    QByteArray inserts;
    int pending = 0;
    const auto flushInserts = [&] {
        if(pending && onStatement)
            onStatement(Kind::Insert, "INSERT INTO " + qualified + " (" + columnList +
                                          ") VALUES\n" + inserts + ";\n");
        inserts.clear();
        pending = 0;
    };

    /* the source's rows stream past them */
    if(!source->streamQuery(select(source, sourceDb), &error, nullptr,
                            [&](const QVector<QByteArray> &f, const QVector<bool> &n) {
                                if(cancelled && cancelled()) {
                                    stopped = true;
                                    return false;
                                }
                                const auto it = targetRows.find(packKey(f, keyColumns));
                                if(it == targetRows.end()) {
                                    ++r.inserts;
                                    if(onStatement) {
                                        QByteArray tuple("(");
                                        for(int i = 0; i < f.size(); ++i)
                                            tuple += (i ? ", " : "") + literal(f, n, i);
                                        inserts += (pending ? ",\n" : "") + tuple + ')';
                                        if(++pending >= rowsPerInsert)
                                            flushInserts();
                                    }
                                    return true;
                                }
                                if(*it == digest(f, n, keyColumns)) {
                                    ++r.same;
                                } else {
                                    ++r.updates;
                                    if(onStatement) {
                                        QByteArray set;
                                        for(int i = keyColumns; i < f.size(); ++i)
                                            set += (i > keyColumns ? ", " : "") +
                                                   target->quoteIdent(r.columns[i]).toUtf8() +
                                                   " = " + literal(f, n, i);
                                        onStatement(Kind::Update, "UPDATE " + qualified + " SET " +
                                                                      set + " WHERE " + where(f) +
                                                                      ";\n");
                                    }
                                }
                                targetRows.erase(it);
                                return true;
                            })) {
        r.skipped = error;
        return r;
    }
    if(stopped) {
        r.skipped = QStringLiteral("cancelled");
        return r;
    }
    flushInserts();

    /* what's left is in the target only */
    r.deletes = targetRows.size();
    if(onStatement && !targetRows.isEmpty()) {
        QList<QByteArray> keys = targetRows.keys();
        std::sort(keys.begin(), keys.end()); /* the same script for the same data */
        if(keyColumns == 1) {
            const QByteArray column = target->quoteIdent(r.key.first()).toUtf8();
            for(qsizetype at = 0; at < keys.size(); at += rowsPerInsert) {
                QByteArray in;
                for(qsizetype i = at; i < std::min(keys.size(), at + rowsPerInsert); ++i)
                    in += (i > at ? ", " : "") + literal(unpackKey(keys[i]), {}, 0);
                onStatement(Kind::Delete, "DELETE FROM " + qualified + " WHERE " + column +
                                              " IN (" + in + ");\n");
            }
        } else {
            for(const QByteArray &k : std::as_const(keys))
                onStatement(Kind::Delete,
                            "DELETE FROM " + qualified + " WHERE " + where(unpackKey(k)) + ";\n");
        }
    }
    return r;
}

bool writeScript(IDbConnection *source, const QString &sourceDb, IDbConnection *target,
                 const QString &targetDb, const QList<Pick> &picks,
                 const std::function<void(const QByteArray &)> &out,
                 const std::function<bool()> &cancelled, QString *error)
{
    QTemporaryFile updates, inserts;
    if(!updates.open() || !inserts.open()) {
        if(error)
            *error = QStringLiteral("can't create a temporary file");
        return false;
    }
    QList<QByteArray> deletes; /* per table, written in reverse */
    for(const Pick &p : picks) {
        QByteArray tableDeletes;
        const TableResult r = compareTable(
            source, sourceDb, target, targetDb, p.table,
            [&](Kind kind, const QByteArray &statement) {
                if(kind == Kind::Delete && p.deletes)
                    tableDeletes += statement;
                else if(kind == Kind::Update && p.updates)
                    updates.write(statement);
                else if(kind == Kind::Insert && p.inserts)
                    inserts.write(statement);
            },
            100, cancelled);
        /* a table without a key to match by has nothing to write; one
         * that has a key failed (or was cancelled) half way: no script */
        if(!r.skipped.isEmpty() && !r.key.isEmpty()) {
            if(error)
                *error = QStringLiteral("%1: %2").arg(p.table, r.skipped);
            return false;
        }
        deletes.prepend(tableDeletes);
    }

    const SqlDriverType driver = target->driverType();
    QByteArray head = QStringLiteral("-- Data sync: make the rows of %1 match %2\n")
                          .arg(targetDb, sourceDb)
                          .toUtf8();
    switch(driver) {
        case SqlDriverType::Mysql:
            head += "SET FOREIGN_KEY_CHECKS = 0;\n";
            break;
        case SqlDriverType::Postgres:
            head += "BEGIN;\n";
            break;
        case SqlDriverType::Sqlite:
            head += "PRAGMA foreign_keys = OFF;\nBEGIN;\n";
            break;
    }
    out(head);
    for(const QByteArray &d : std::as_const(deletes))
        if(!d.isEmpty())
            out(d);
    for(QTemporaryFile *f : {&updates, &inserts}) {
        f->seek(0);
        while(!f->atEnd())
            out(f->read(1 << 20));
    }
    switch(driver) {
        case SqlDriverType::Mysql:
            out("SET FOREIGN_KEY_CHECKS = 1;\n");
            break;
        case SqlDriverType::Postgres:
            out("COMMIT;\n");
            break;
        case SqlDriverType::Sqlite:
            out("COMMIT;\nPRAGMA foreign_keys = ON;\n");
            break;
    }
    return true;
}

} // namespace DataSync
