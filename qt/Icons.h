/* OpenYog — loads the original GPL SQLyog toolbar/table icons from
 * include/bitmaps (they ship with the fork).
 *
 * They are compiled into the binary as ":/bitmaps/<name>" (CMakeLists.txt globs
 * the directory into a generated bitmaps.qrc). The loose-file path below is
 * kept only as a fallback: it resolves inside a build tree and nowhere else, so
 * before the resource existed a deployed build — the Windows zip, or an
 * installed Linux prefix — handed every call site a null QIcon and the whole UI
 * came up as bare text.
 *
 * Note for Windows deployment: .ico decoding is an imageformats PLUGIN
 * (qico.dll), not built into QtGui the way PNG is. Embedding the files is only
 * half the job — tools/windows-cross-build.sh deploys the plugin too.
 */
#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

namespace Icons {
inline QString dir()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("../include/bitmaps/"));
}

/* a path QIcon and QPixmap both accept — the compiled-in copy when present */
inline QString path(const QString &file)
{
    const QString res = QStringLiteral(":/bitmaps/") + file;
    return QFile::exists(res) ? res : dir() + file;
}

inline QIcon get(const QString &file)
{
    static QHash<QString, QIcon> cache;
    if(!cache.contains(file))
        cache.insert(file, QIcon(path(file)));
    return cache.value(file);
}

/* a handful of the bundled 16x16 .ico files draw their artwork edge-to-edge
 * (no breathing room inside the canvas), so next to any normally-padded
 * sibling icon in the same menu they look flush/clipped against the
 * menu's left border. Re-renders the icon shrunk by marginPx on a
 * transparent 16x16 canvas so it reads consistently with the rest. */
inline QIcon getInset(const QString &file, int marginPx = 2)
{
    const QString key = file + QStringLiteral("@inset%1").arg(marginPx);
    static QHash<QString, QIcon> cache;
    if(cache.contains(key))
        return cache.value(key);
    constexpr int kSize = 16;
    QPixmap canvas(kSize, kSize);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    get(file).paint(&p, QRect(marginPx, marginPx, kSize - 2 * marginPx, kSize - 2 * marginPx));
    p.end();
    const QIcon result(canvas);
    cache.insert(key, result);
    return result;
}
} // namespace Icons
