/* OpenYog — SQL dump writer (Database > Backup/Export > Backup As SQL Dump).
 * Mirrors the essentials of upstream ExportData/ExportBatch: per table an
 * optional DROP, the SHOW CREATE TABLE statement, then batched INSERTs.
 * Runs on the calling thread against the live browsing connection — fine for
 * the sizes a desktop client dumps interactively; a threaded path can come
 * later if it matters. */
#pragma once

#include <QString>
#include <QStringList>

#include <mysql/mysql.h>

class QIODevice;

namespace SqlDump {

struct Options
{
    bool addDropTable = true;
    bool structure    = true;
    bool data         = true;
    int  rowsPerInsert = 100;
};

/* Dump `tables` (or every base table in `db` when empty) into `out`.
 * Returns true on success; on failure returns false and sets *error. */
bool write(MYSQL *conn, const QString &db, const QStringList &tables,
           const Options &opt, QIODevice *out, QString *error);

} // namespace SqlDump
