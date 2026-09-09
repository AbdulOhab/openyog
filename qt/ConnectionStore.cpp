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

    if(wyIni::IniGetString(n, "host", "", &value, iniPath().toUtf8()) == 0 ||
       value.GetLength() == 0)
        return false;

    out->name  = name;
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
    return true;
}

void ConnectionStore::save(const ConnectionParams &params)
{
    const QByteArray n = toUtf8(params.name);
    const QByteArray p = iniPath().toUtf8();

    wyIni::IniWriteString(n, "host",     toUtf8(params.host),     p);
    wyIni::IniWriteString(n, "user",     toUtf8(params.user),     p);
    wyIni::IniWriteString(n, "database", toUtf8(params.database), p);
    wyIni::IniWriteInt   (n, "port",     params.port,             p);

    wyChar *b64 = NULL;
    const QByteArray pw = toUtf8(params.password);
    EncodeBase64(pw.constData(), pw.size(), &b64);
    wyIni::IniWriteString(n, "password", b64 ? b64 : "", p);
    free(b64);
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
