/* OpenYog — MySQL/MariaDB IDbDriver implementation. */
#pragma once

#include "IDbDriver.h"

class MySqlDriver : public IDbDriver
{
public:
    QString driverName() const override
    {
        return QStringLiteral("MySQL");
    }
    int defaultPort() const override
    {
        return 3306;
    }

    IDbConnection *connect(const ConnectionParams &params, QString *error,
                           bool localInfile = false) override;

    void libraryInit() override;
    void libraryShutdown() override;
};
