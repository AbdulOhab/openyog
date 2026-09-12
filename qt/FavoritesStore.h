/* OpenYog — persistent query favorites/snippets store.
 * Built on the same portable core (wyIni + EncodeBase64) as
 * ConnectionStore, storing arbitrary multi-line SQL text base64-encoded so
 * newlines/special characters survive the INI format unscathed. */
#pragma once

#include <QString>
#include <QStringList>

namespace FavoritesStore
{
    /* Names of all saved favorites, in the order the file lists them. */
    QStringList names();

    /* Returns the saved SQL text for `name`, or an empty string if absent. */
    QString get(const QString &name);

    /* Writes (replaces) one favorite. */
    void save(const QString &name, const QString &sql);

    /* Deletes one favorite. No-op if it doesn't exist. */
    void remove(const QString &name);

    /* Renames a favorite (load → save under newName → remove old).
     * Returns false if `oldName` doesn't exist or `newName` is empty. */
    bool rename(const QString &oldName, const QString &newName);
}
