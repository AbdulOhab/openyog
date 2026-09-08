/* OpenYog — persistent connection store.
 *
 * Deliberately built on the ported core (wyIni + EncodeBase64), i.e. the same
 * INI-with-encoded-password scheme SQLyog itself used for saved connections —
 * kept for authenticity; base64 is obfuscation, not encryption (same as
 * upstream).
 */
#pragma once

#include "ConnectionParams.h"

namespace ConnectionStore
{
    /* Returns false if no entry with that name exists. */
    bool load(const QString &name, ConnectionParams *out);

    /* Writes (replaces) one section. */
    void save(const ConnectionParams &params);

    /* Names of all stored sections that look like connections. */
    QStringList storedNames();
}
