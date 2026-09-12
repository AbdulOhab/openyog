/* OpenYog — SQL dump writer (Database > Backup/Export > Backup As SQL Dump).
 * Mirrors the essentials of upstream ExportData/ExportBatch: per table an
 * optional DROP, the SHOW CREATE TABLE statement, then batched INSERTs.
 * Runs on the calling thread against the live browsing connection — fine for
 * the sizes a desktop client dumps interactively; a threaded path can come
 * later if it matters. */
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class IDbConnection;
class QIODevice;

namespace SqlDump {

struct Options
{
    bool addDropTable = true;
    bool structure    = true;
    bool data         = true;
    bool routines     = false;   /* forEachStatement: also views/procs/funcs/
                                  * triggers/events (DEFINER stripped) */
    int  rowsPerInsert = 100;
};

/* Dump `tables` (or every base table in `db` when empty) into `out`.
 * Returns true on success; on failure returns false and sets *error. */
bool write(IDbConnection *conn, const QString &db, const QStringList &tables,
           const Options &opt, QIODevice *out, QString *error);

/* The database as one statement at a time (no trailing ';', no comments):
 * base tables (CREATE from SHOW CREATE TABLE, so constraints/FKs survive) +
 * batched INSERTs, and — with opt.routines — views / procedures / functions /
 * triggers / events (DEFINER stripped, source-db qualifier removed; the caller
 * must have USE'd the target). Handed to `exec` to replay onto another conn.
 * Stops and returns false if `exec` returns false (then *error is whatever
 * exec left, or unset). */
bool forEachStatement(IDbConnection *conn, const QString &db, const QStringList &tables,
                      const Options &opt,
                      const std::function<bool(const QString &stmt)> &exec,
                      QString *error);

} // namespace SqlDump
