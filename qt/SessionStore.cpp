#include "SessionStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
QString sessionPath()
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    dir.mkpath(QStringLiteral("."));
    return dir.filePath(QStringLiteral("session.json"));
}

QJsonObject load()
{
    QFile f(sessionPath());
    if(!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

/* via QSaveFile: a crash mid-write leaves the previous session intact */
void save(const QJsonObject &root)
{
    QSaveFile f(sessionPath());
    if(!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(root).toJson());
    f.commit();
}
} // namespace

namespace SessionStore {

QList<EditorState> editors(const QString &connection)
{
    QList<EditorState> out;
    const QJsonArray tabs =
        load().value(QStringLiteral("editors")).toObject().value(connection).toArray();
    for(const QJsonValue &v : tabs) {
        const QJsonObject o = v.toObject();
        out.append({o.value(QStringLiteral("title")).toString(),
                    o.value(QStringLiteral("text")).toString(),
                    o.value(QStringLiteral("cursor")).toInt()});
    }
    return out;
}

void setEditors(const QString &connection, const QList<EditorState> &editors)
{
    if(connection.isEmpty())
        return;
    QJsonArray tabs;
    for(const EditorState &e : editors)
        if(!e.text.trimmed().isEmpty())
            tabs.append(QJsonObject{{QStringLiteral("title"), e.title},
                                    {QStringLiteral("text"), e.text},
                                    {QStringLiteral("cursor"), e.cursor}});
    QJsonObject root = load();
    QJsonObject all = root.value(QStringLiteral("editors")).toObject();
    if(tabs.isEmpty())
        all.remove(connection);
    else
        all.insert(connection, tabs);
    root.insert(QStringLiteral("editors"), all);
    save(root);
}

QStringList openConnections()
{
    QStringList out;
    for(const QJsonValue &v : load().value(QStringLiteral("open")).toArray())
        out << v.toString();
    return out;
}

void setOpenConnections(const QStringList &names)
{
    QJsonObject root = load();
    root.insert(QStringLiteral("open"), QJsonArray::fromStringList(names));
    save(root);
}

bool restoreOnStartup()
{
    return load().value(QStringLiteral("restoreOnStartup")).toBool(true);
}

void setRestoreOnStartup(bool on)
{
    QJsonObject root = load();
    root.insert(QStringLiteral("restoreOnStartup"), on);
    save(root);
}

} // namespace SessionStore
