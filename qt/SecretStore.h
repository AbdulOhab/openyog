/* OpenYog — the OS credential store, through QtKeychain: Secret Service
 * (GNOME Keyring, KeePassXC …) or KWallet on Linux, Credential Manager on
 * Windows, Keychain on macOS.
 *
 * Synchronous on purpose: each call runs its QtKeychain job to completion in
 * a local event loop (user input excluded, so nothing re-enters the caller),
 * keeping ConnectionStore's API as plain as the INI store underneath it.
 * GUI thread only.
 *
 * Built without QtKeychain (OPENYOG_HAVE_KEYCHAIN undefined, e.g. the mingw
 * cross build) every call just fails, and ConnectionStore falls back to its
 * INI field. */
#pragma once

#include <QString>

namespace SecretStore {
/* false: no secret stored under `key`, no backend, or access denied */
bool read(const QString &key, QString *secret);

/* false: no backend answered or it refused; nothing was stored */
bool write(const QString &key, const QString &secret);

/* deletes the entry; a missing one counts as success */
bool remove(const QString &key);
} // namespace SecretStore
