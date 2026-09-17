/* OpenYog — loads the original GPL SQLyog toolbar/table icons from
 * include/bitmaps (they ship with the fork). */
#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QIcon>

namespace Icons {
inline QString dir()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("../include/bitmaps/"));
}

inline QIcon get(const QString &file)
{
    static QHash<QString, QIcon> cache;
    if(!cache.contains(file))
        cache.insert(file, QIcon(dir() + file));
    return cache.value(file);
}
} // namespace Icons
