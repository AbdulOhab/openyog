/* OpenYog — persistent connection store.
 *
 * Built on the ported core (wyIni), i.e. the same connections.ini SQLyog
 * itself used. Passwords go to the OS keychain (SecretStore) and the section
 * only records `password_in_keychain=1`; upstream's base64 `password` field
 * (obfuscation, not encryption) is the fallback when no keychain backend
 * takes the secret, and an existing base64 password moves into the keychain
 * the first time its connection is loaded.
 */
#pragma once

#include "ConnectionParams.h"

namespace ConnectionStore {
/* Returns false if no entry with that name exists. */
bool load(const QString &name, ConnectionParams *out);

/* Writes (replaces) one section. */
void save(const ConnectionParams &params);

/* Names of all stored sections that look like connections. */
QStringList storedNames();

/* Deletes one stored connection. No-op if it doesn't exist. */
void remove(const QString &name);

/* Renames a stored connection (load → save under newName → remove old).
 * Returns false if `oldName` doesn't exist. */
bool rename(const QString &oldName, const QString &newName);
} // namespace ConnectionStore
