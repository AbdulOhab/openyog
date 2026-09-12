#include "FavoritesStore.h"

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
    return dir.filePath("favorites.ini");
}

QStringList FavoritesStore::names()
{
    QFile f(iniPath());
    if(!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QStringList out;
    QTextStream in(&f);
    for(const QString &line : in.readAll().split('\n')) {
        const QString t = line.trimmed();
        if(t.startsWith('[') && t.endsWith(']'))
            out << t.mid(1, t.size() - 2);
    }
    return out;
}

QString FavoritesStore::get(const QString &name)
{
    wyString value;
    if(wyIni::IniGetString(name.toUtf8(), "sql", "", &value, iniPath().toUtf8()) == 0
       || value.GetLength() == 0)
        return {};
    const QByteArray b64(value.GetString());
    QByteArray decoded(b64.size() * 3 / 4 + 1, '\0');
    const size_t rawlen = DecodeBase64(b64.constData(), decoded.data());
    decoded.resize((int)rawlen);
    return QString::fromUtf8(decoded);
}

void FavoritesStore::save(const QString &name, const QString &sql)
{
    const QByteArray n = name.toUtf8();
    const QByteArray p = iniPath().toUtf8();
    wyChar *b64 = nullptr;
    const QByteArray raw = sql.toUtf8();
    EncodeBase64(raw.constData(), raw.size(), &b64);
    wyIni::IniWriteString(n, "sql", b64 ? b64 : "", p);
    free(b64);
}

void FavoritesStore::remove(const QString &name)
{
    wyIni::IniDeleteSection(name.toUtf8().constData(), iniPath().toUtf8());
}

bool FavoritesStore::rename(const QString &oldName, const QString &newName)
{
    if(newName.trimmed().isEmpty() || oldName == newName)
        return false;
    const QString sql = get(oldName);
    if(sql.isEmpty() && !names().contains(oldName))
        return false;
    save(newName, sql);
    remove(oldName);
    return true;
}
