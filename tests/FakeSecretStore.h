/* OpenYog tests — an in-memory stand-in for qt/SecretStore.cpp, linked in its
 * place so ConnectionStore's keychain paths run without a Secret Service. */
#pragma once

#include <QHash>
#include <QString>

namespace FakeSecretStore {
/* forget every entry; `available` = whether reads/writes succeed (false
 * behaves like a build or desktop with no keychain backend) */
void reset(bool available = true);

/* flip availability but keep the entries (a keychain that got locked) */
void setAvailable(bool available);

QHash<QString, QString> &entries();
} // namespace FakeSecretStore
