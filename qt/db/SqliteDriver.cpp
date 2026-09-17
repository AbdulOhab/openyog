#include "SqliteDriver.h"
#include "SqliteConnection.h"

#include <sqlite3.h>

IDbConnection *SqliteDriver::connect(const ConnectionParams &params, QString *error,
                                     bool /*localInfile*/)
{
    if(params.filePath.trimmed().isEmpty()) {
        if(error)
            *error = QStringLiteral("no file path specified");
        return nullptr;
    }
    sqlite3 *db = nullptr;
    const int rc = sqlite3_open_v2(params.filePath.toUtf8().constData(), &db,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if(rc != SQLITE_OK) {
        if(error)
            *error = db ? QString::fromUtf8(sqlite3_errmsg(db))
                        : QStringLiteral("could not open database file");
        if(db)
            sqlite3_close(db);
        return nullptr;
    }
    return new SqliteConnection(db);
}
