/* OpenYog — what Data Sync (Powertools ▸ Database Synchronization Wizard)
 * compares and writes: the rows of the tables a source and a target database
 * of the same kind share, matched by primary key, and the INSERT / UPDATE /
 * DELETE script that makes the target's rows match the source's.
 *
 * Big tables stay small in memory: the target's rows are read first and only
 * their key and a digest of the rest are kept, then the source's stream past
 * them. Only the columns both tables have are compared, value by value as the
 * server writes them. The script, like Schema Sync's, is for the user to read
 * and run; nothing here changes the target. */
#pragma once

#include "ConnectionParams.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

class IDbConnection;

namespace DataSync {

enum class Kind {
    Insert, /* in the source only */
    Update, /* in both, different */
    Delete  /* in the target only */
};

struct TableResult
{
    QString table;
    QStringList columns; /* compared: both tables have them; the key first */
    QStringList key;     /* the primary key */
    QStringList ignored; /* columns only one of the two tables has */
    qint64 inserts = 0, updates = 0, deletes = 0, same = 0;
    QString skipped; /* why it wasn't compared (no primary key, an error…) */
};

/* the base tables both databases have, each after the tables it references
 * (the target's foreign keys): the order inserts run in, deletes the reverse */
QStringList commonTables(IDbConnection *source, const QString &sourceDb, IDbConnection *target,
                         const QString &targetDb);

/* one statement, ";\n"-terminated */
using Emit = std::function<void(Kind kind, const QByteArray &statement)>;

/* compares one table; with `onStatement` also writes its statements as it goes (an
 * INSERT per `rowsPerInsert` rows, a DELETE … IN (…) as many for a one-column
 * key); `cancelled` is asked between rows */
TableResult compareTable(IDbConnection *source, const QString &sourceDb, IDbConnection *target,
                         const QString &targetDb, const QString &table,
                         const Emit &onStatement = {}, int rowsPerInsert = 100,
                         const std::function<bool()> &cancelled = {});

/* a table to bring over, and which of its changes */
struct Pick
{
    QString table;
    bool inserts = true, updates = true, deletes = false;
};

/* the whole script for `picks`, given in commonTables() order, handed to
 * `out` piece by piece: deletes first (a table before the tables it
 * references), then updates, then inserts (the referenced tables first), so
 * it runs with foreign keys on; foreign key checks are off around it anyway
 * where the dialect allows (MySQL, SQLite) and PostgreSQL's and SQLite's run
 * as one transaction. Updates and inserts wait in temporary files, not in
 * memory. False on an error or when cancelled, with *error saying which. */
bool writeScript(IDbConnection *source, const QString &sourceDb, IDbConnection *target,
                 const QString &targetDb, const QList<Pick> &picks,
                 const std::function<void(const QByteArray &)> &out,
                 const std::function<bool()> &cancelled = {}, QString *error = nullptr);

} // namespace DataSync
