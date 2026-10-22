/* OpenYog — keeping what it stores to the user who stores it. Saved
 * connections (passwords too, base64 in connections.ini when no keychain
 * takes them), the query history and the last session's query tabs live
 * under the user's config and data folders; created with the default
 * umask, those folders and files were readable by every account on the
 * machine. On Unix they are now the owner's alone: 0700 folders, 0600
 * files. Windows already keeps a user's AppData folders private; there this
 * does nothing. */
#pragma once

#include <QString>

namespace PrivateFiles {

/* the app's config and data folders 0700 and every file in them 0600 —
 * once at startup, which also covers files left by earlier versions; needs
 * the application and organization names set */
void protectAppFolders();

/* one file 0600, right after writing it (a file created while the app runs
 * gets the umask's mode until then) */
void restrict(const QString &path);

} // namespace PrivateFiles
