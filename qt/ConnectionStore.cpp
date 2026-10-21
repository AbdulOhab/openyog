#include "ConnectionStore.h"

#include "SecretStore.h"
#include "wyIni.h"
#include "CommonHelper.h" /* EncodeBase64/DecodeBase64 + wyString (port shim) */

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

static QByteArray toUtf8(const QString &s)
{
    return s.toUtf8();
}

/* the connection's entry in the OS keychain (SecretStore) */
static QString secretKey(const QString &name)
{
    return QStringLiteral("connection/") + name;
}

/* upstream's INI password field: base64, i.e. obfuscation, not encryption */
static QString decodeIniPassword(wyString &b64value) /* wyString getters aren't const */
{
    if(b64value.GetLength() == 0)
        return QString();
    QByteArray b64(b64value.GetString());
    QByteArray decoded(b64.size() * 3 / 4 + 1, '\0');
    size_t rawlen = DecodeBase64(b64.constData(), decoded.data());
    decoded.resize((int)rawlen);
    return QString::fromUtf8(decoded);
}

static void writeIniPassword(const QByteArray &section, const QString &password,
                             const QByteArray &iniFile)
{
    wyChar *b64 = NULL;
    const QByteArray pw = toUtf8(password);
    EncodeBase64(pw.constData(), pw.size(), &b64);
    wyIni::IniWriteString(section, "password", b64 ? b64 : "", iniFile);
    free(b64);
}

/* move the password into the keychain and blank the INI copy; false (and
 * the INI left as it was) when no keychain backend takes it */
static bool storeInKeychain(const QString &name, const QString &password, const QByteArray &iniFile)
{
    if(!SecretStore::write(secretKey(name), password))
        return false;
    const QByteArray n = toUtf8(name);
    wyIni::IniWriteString(n, "password", "", iniFile);
    wyIni::IniWriteInt(n, "password_in_keychain", 1, iniFile);
    return true;
}

bool ConnectionStore::load(const QString &name, ConnectionParams *out)
{
    const QByteArray n = toUtf8(name);
    wyString value;

    /* "driver" is new — old files without it default to MySQL */
    wyIni::IniGetString(n, "driver", "mysql", &value, iniPath().toUtf8());
    const SqlDriverType driver = driverTypeFromString(QString::fromUtf8(value.GetString()));

    wyIni::IniGetString(n, "filepath", "", &value, iniPath().toUtf8());
    const QString filePath = value.GetString();

    if(driver == SqlDriverType::Sqlite) {
        if(filePath.isEmpty())
            return false;
        out->driverType = SqlDriverType::Sqlite;
        out->filePath = filePath;
        out->name = name;
        return true;
    }

    if(wyIni::IniGetString(n, "host", "", &value, iniPath().toUtf8()) == 0 ||
       value.GetLength() == 0)
        return false;

    out->name = name;
    out->driverType = driver; /* Mysql or Postgres — same field shape */
    out->host = value.GetString();

    wyIni::IniGetString(n, "user", "", &value, iniPath().toUtf8());
    out->user = value.GetString();
    wyIni::IniGetString(n, "database", "", &value, iniPath().toUtf8());
    out->database = value.GetString();
    out->port = wyIni::IniGetInt(n, "port", driver == SqlDriverType::Postgres ? 5432 : 3306,
                                 iniPath().toUtf8());

    /* password: in the OS keychain when the entry says so; otherwise the
     * INI's base64 field (built without a keychain, none answered at save
     * time, or a file from before keychain support), which gets moved into
     * the keychain now if one takes it */
    out->password.clear();
    if(wyIni::IniGetInt(n, "password_in_keychain", 0, iniPath().toUtf8()) != 0) {
        /* missing / locked / denied: connect with an empty password and let
         * the server's own error say so */
        SecretStore::read(secretKey(name), &out->password);
    } else {
        wyIni::IniGetString(n, "password", "", &value, iniPath().toUtf8());
        out->password = decodeIniPassword(value);
        if(!out->password.isEmpty())
            storeInKeychain(name, out->password, iniPath().toUtf8());
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

    if(params.driverType == SqlDriverType::Sqlite) {
        wyIni::IniWriteString(n, "filepath", toUtf8(params.filePath), p);
        return;
    }

    wyIni::IniWriteString(n, "host", toUtf8(params.host), p);
    wyIni::IniWriteString(n, "user", toUtf8(params.user), p);
    wyIni::IniWriteString(n, "database", toUtf8(params.database), p);
    wyIni::IniWriteInt(n, "port", params.port, p);

    /* keychain first; the INI field only when no keychain takes it, never
     * both */
    if(!storeInKeychain(params.name, params.password, p)) {
        writeIniPassword(n, params.password, p);
        wyIni::IniWriteInt(n, "password_in_keychain", 0, p);
    }

    wyIni::IniWriteInt(n, "use_ssl", params.useSsl ? 1 : 0, p);
    wyIni::IniWriteString(n, "ssl_ca", toUtf8(params.sslCa), p);
    wyIni::IniWriteString(n, "ssl_cert", toUtf8(params.sslCert), p);
    wyIni::IniWriteString(n, "ssl_key", toUtf8(params.sslKey), p);

    wyIni::IniWriteInt(n, "compress", params.compress ? 1 : 0, p);
    wyIni::IniWriteInt(n, "idle_timeout_secs", params.idleTimeoutSecs, p);
    wyIni::IniWriteInt(n, "keepalive_secs", params.keepAliveSecs, p);
}

void ConnectionStore::remove(const QString &name)
{
    const QByteArray n = toUtf8(name);
    /* only touch the keychain for entries that use it: a delete there can
     * mean an unlock prompt */
    if(wyIni::IniGetInt(n, "password_in_keychain", 0, iniPath().toUtf8()) != 0)
        SecretStore::remove(secretKey(name));
    wyIni::IniDeleteSection(n.constData(), iniPath().toUtf8());
}

bool ConnectionStore::rename(const QString &oldName, const QString &newName)
{
    if(oldName == newName || newName.trimmed().isEmpty())
        return false;
    ConnectionParams p;
    if(!load(oldName, &p))
        return false;
    /* a keychain that won't hand the password over (locked, access denied)
     * leaves p.password empty, and the save + remove below would then lose
     * it for good: refuse the rename instead */
    QString secret;
    if(wyIni::IniGetInt(toUtf8(oldName), "password_in_keychain", 0, iniPath().toUtf8()) != 0 &&
       !SecretStore::read(secretKey(oldName), &secret))
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
