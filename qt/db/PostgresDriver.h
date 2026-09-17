/* OpenYog — PostgreSQL IDbDriver implementation (libpq). */
#pragma once

#include "IDbDriver.h"

class PostgresDriver : public IDbDriver
{
public:
    QString driverName() const override
    {
        return QStringLiteral("PostgreSQL");
    }
    int defaultPort() const override
    {
        return 5432;
    }

    IDbConnection *connect(const ConnectionParams &params, QString *error,
                           bool localInfile = false) override;

    /* libpq self-initializes on first use; no global handle to release
     * (unlike mysql_library_init/_end). */
    void libraryInit() override {}
    void libraryShutdown() override {}
};
