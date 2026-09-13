#include "ConnectionStore.h"

#include "wyIni.h"
#include "CommonHelper.h"   /* EncodeBase64/DecodeBase64 + wyString (port shim) */

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

static QString iniPath()
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    dir.mkpath(".");
    return dir.filePath("connections.ini");
}

static QByteArray toUtf8(const QString &s) { return s.toUtf8(); }

bool ConnectionStore::load(const QString &name, ConnectionParams *out)
{
    const QByteArray n = toUtf8(name);
    wyString value;

    /* "driver" is new — old files without it default to MySQL */
    wyIni::IniGetString(n, "driver", "mysql", &value, iniPath().toUtf8());
    const DriverType driver = driverTypeFromString(QString::fromUtf8(value.GetString()));

    wyIni::IniGetString(n, "filepath", "", &value, iniPath().toUtf8());
    const QString filePath = value.GetString();

    if(driver == DriverType::Sqlite) {
        if(filePath.isEmpty())
            return false;
        out->driverType = DriverType::Sqlite;
        out->filePath   = filePath;
        out->name       = name;
        return true;
    }

    if(wyIni::IniGetString(n, "host", "", &value, iniPath().toUtf8()) == 0 ||
       value.GetLength() == 0)
        return false;

    out->name       = name;
    out->driverType = DriverType::Mysql;
    out->host  = value.GetString();

    wyIni::IniGetString(n, "user", "", &value, iniPath().toUtf8());
    out->user  = value.GetString();
    wyIni::IniGetString(n, "database", "", &value, iniPath().toUtf8());
    out->database = value.GetString();
    out->port = wyIni::IniGetInt(n, "port", 3306, iniPath().toUtf8());

    /* password is stored base64-encoded, like upstream connection files */
    wyIni::IniGetString(n, "password", "", &value, iniPath().toUtf8());
    out->password.clear();
    if(value.GetLength() > 0) {
        QByteArray b64(value.GetString());
        QByteArray decoded(b64.size() * 3 / 4 + 1, '\0');
        size_t rawlen = DecodeBase64(b64.constData(), decoded.data());
        decoded.resize((int)rawlen);
        out->password = QString::fromUtf8(decoded);
    }

    out->useSsl = wyIni::IniGetInt(n, "use_ssl", 0, iniPath().toUtf8()) != 0;
    wyIni::IniGetString(n, "ssl_ca", "", &value, iniPath().toUtf8());
    out->sslCa = value.GetString();
    wyIni::IniGetString(n, "ssl_cert", "", &value, iniPath().toUtf8());
    out->sslCert = value.GetString();
    wyIni::IniGetString(n, "ssl_key", "", &value, iniPath().toUtf8());
    out->sslKey = value.GetString();

    out->compress = wyIni::IniGetInt(n, "compress", 0, iniPath().toUtf8()) != 0;
    out->idleTimeoutSecs = wyIni::IniGetInt(n, "idle_timeout_secs", 0, iniPath().toUtf8());
    out->keepAliveSecs = wyIni::IniGetInt(n, "keepalive_secs", 0, iniPath().toUtf8());
    return true;
}

void ConnectionStore::save(const ConnectionParams &params)
{
    const QByteArray n = toUtf8(params.name);
    const QByteArray p = iniPath().toUtf8();

    wyIni::IniWriteString(n, "driver", toUtf8(driverTypeToString(params.driverType)), p);

    if(params.driverType == DriverType::Sqlite) {
        wyIni::IniWriteString(n, "filepath", toUtf8(params.filePath), p);
        return;
    }

    wyIni::IniWriteString(n, "host",     toUtf8(params.host),     p);
    wyIni::IniWriteString(n, "user",     toUtf8(params.user),     p);
    wyIni::IniWriteString(n, "database", toUtf8(params.database), p);
    wyIni::IniWriteInt   (n, "port",     params.port,             p);

    wyChar *b64 = NULL;
    const QByteArray pw = toUtf8(params.password);
    EncodeBase64(pw.constData(), pw.size(), &b64);
    wyIni::IniWriteString(n, "password", b64 ? b64 : "", p);
    free(b64);

    wyIni::IniWriteInt   (n, "use_ssl",  params.useSsl ? 1 : 0, p);
    wyIni::IniWriteString(n, "ssl_ca",   toUtf8(params.sslCa),   p);
    wyIni::IniWriteString(n, "ssl_cert", toUtf8(params.sslCert), p);
    wyIni::IniWriteString(n, "ssl_key",  toUtf8(params.sslKey),  p);

    wyIni::IniWriteInt(n, "compress", params.compress ? 1 : 0, p);
    wyIni::IniWriteInt(n, "idle_timeout_secs", params.idleTimeoutSecs, p);
    wyIni::IniWriteInt(n, "keepalive_secs", params.keepAliveSecs, p);
}

void ConnectionStore::remove(const QString &name)
{
    wyIni::IniDeleteSection(toUtf8(name).constData(), iniPath().toUtf8());
}

bool ConnectionStore::rename(const QString &oldName, const QString &newName)
{
    if(oldName == newName || newName.trimmed().isEmpty())
        return false;
    ConnectionParams p;
    if(!load(oldName, &p))
        return false;
    p.name = newName;
    save(p);
    remove(oldName);
    return true;
}

QStringList ConnectionStore::storedNames()
{
    /* NOTE: upstream wyIni::IniGetSection/GetAllSectionDetails only reports
     * sections whose name contains the word "Connection" (SQLyog's own
     * naming scheme, see port-gaps note). Our section names are user-facing
     * strings, so we list section headers by reading the INI directly. */
    QFile f(iniPath());
    if(!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QStringList names;
    QTextStream in(&f);
    for(const QString &line : in.readAll().split('\n')) {
        const QString t = line.trimmed();
        if(t.startsWith('[') && t.endsWith(']'))
            names << t.mid(1, t.size() - 2);
    }
    return names;
}
