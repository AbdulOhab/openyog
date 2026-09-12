/* OpenYog — database-backend factory. One IDbDriver per backend (MySQL now;
 * SQLite/PostgreSQL are added later as new files behind this same interface,
 * with zero changes to any UI file in qt/). */
#pragma once

#include "../ConnectionParams.h"

#include <QString>

class IDbConnection;

class IDbDriver
{
public:
    virtual ~IDbDriver() = default;

    virtual QString driverName() const = 0;
    virtual int     defaultPort() const = 0;

    /* Opens one connection. Caller owns the returned pointer (delete it to
     * disconnect). Returns nullptr and fills *error on failure. `localInfile`
     * enables LOAD DATA LOCAL INFILE — only the GUI-thread browsing
     * connection needs it today (CSV/XML import). */
    virtual IDbConnection *connect(const ConnectionParams &params, QString *error,
                                   bool localInfile = false) = 0;

    /* Process-lifetime hooks, called once from main() around the whole
     * application run — not per-connection. */
    virtual void libraryInit() = 0;
    virtual void libraryShutdown() = 0;
};

/* The single place a new backend gets wired in. */
IDbDriver *dbDriverFor(DriverType type);
