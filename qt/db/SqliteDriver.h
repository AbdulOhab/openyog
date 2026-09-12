/* OpenYog — SQLite IDbDriver implementation. */
#pragma once

#include "IDbDriver.h"

class SqliteDriver : public IDbDriver
{
public:
    QString driverName() const override { return QStringLiteral("SQLite"); }
    int     defaultPort() const override { return 0; }   /* unused: file-based */

    IDbConnection *connect(const ConnectionParams &params, QString *error,
                           bool localInfile = false) override;

    /* SQLite auto-initializes; no global handle to release. */
    void libraryInit() override {}
    void libraryShutdown() override {}
};
