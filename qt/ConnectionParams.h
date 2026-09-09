/* OpenYog — connection parameter type shared by
 * the connection dialog, the store and the connection tab. */
#pragma once

#include <QString>

struct ConnectionParams
{
    QString name     = "New connection";
    QString host     = "127.0.0.1";
    int     port     = 3306;
    QString user;
    QString password;
    QString database;   // optional
};
