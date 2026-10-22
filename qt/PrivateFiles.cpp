/* OpenYog — owner-only folders and files; see PrivateFiles.h. */
#include "PrivateFiles.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace PrivateFiles {

void restrict(const QString &path)
{
#ifdef Q_OS_UNIX
    if(QFile::exists(path))
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
#else
    Q_UNUSED(path);
#endif
}

void protectAppFolders()
{
#ifdef Q_OS_UNIX
    for(const auto location :
        {QStandardPaths::AppConfigLocation, QStandardPaths::AppDataLocation}) {
        const QString path = QStandardPaths::writableLocation(location);
        if(path.isEmpty() || !QDir().mkpath(path))
            continue;
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        for(const QString &file : QDir(path).entryList(QDir::Files | QDir::Hidden))
            restrict(QDir(path).filePath(file));
    }
#endif
}

} // namespace PrivateFiles
