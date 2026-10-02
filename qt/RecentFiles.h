/* OpenYog — File > Recent Files list: the last kMax SQL files opened or
 * saved, persisted in OpenYog.ini [Files] as numbered keys, newest first
 * (the same shape upstream sqlyog.ini used — FrameWindow::WriteLatestFile
 * wrote numbered keys and InsertRecentMenuItems read them back as "&N
 * path" rows, ten max). One shared load/push so every open path records
 * and the menu always reflects the file.
 *
 * The ini file is the same one Theme (themePath) and ObjectBrowser
 * (settingsIniPath) use; each keeps its own small accessor locally.
 */
#pragma once

#include <QDir>
#include <QStandardPaths>

#include "wyIni.h"

namespace RecentFiles {
constexpr int kMax = 10;

inline QByteArray iniPath()
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    dir.mkpath(".");
    return dir.filePath(QStringLiteral("OpenYog.ini")).toUtf8();
}

inline QStringList load()
{
    QStringList out;
    const QByteArray ini = iniPath();
    for(int i = 1; i <= kMax; ++i) {
        wyString v;
        wyIni::IniGetString("Files", QByteArray::number(i).constData(), "", &v, ini);
        const QString s = QString::fromUtf8(v.GetString());
        if(!s.isEmpty())
            out.append(s);
    }
    return out;
}

inline void push(const QString &file)
{
    if(file.isEmpty())
        return;
    QStringList files = load();
    files.removeAll(file); /* re-opening moves it to the front, no dupes */
    files.prepend(file);
    while(files.size() > kMax)
        files.removeLast();
    const QByteArray ini = iniPath();
    for(int i = 0; i < kMax; ++i) /* write all ten: stale tail keys must go */
        wyIni::IniWriteString("Files", QByteArray::number(i + 1).constData(),
                              (i < files.size() ? files.at(i) : QString()).toUtf8(), ini);
}
} // namespace RecentFiles
