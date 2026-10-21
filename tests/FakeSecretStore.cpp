#include "FakeSecretStore.h"

#include "SecretStore.h"

namespace {
bool g_available = true;
}

namespace FakeSecretStore {
void reset(bool available)
{
    g_available = available;
    entries().clear();
}

void setAvailable(bool available)
{
    g_available = available;
}

QHash<QString, QString> &entries()
{
    static QHash<QString, QString> e;
    return e;
}
} // namespace FakeSecretStore

namespace SecretStore {
bool read(const QString &key, QString *secret)
{
    if(!g_available || !FakeSecretStore::entries().contains(key))
        return false;
    *secret = FakeSecretStore::entries().value(key);
    return true;
}

bool write(const QString &key, const QString &secret)
{
    if(!g_available)
        return false;
    FakeSecretStore::entries().insert(key, secret);
    return true;
}

bool remove(const QString &key)
{
    if(!g_available)
        return false;
    FakeSecretStore::entries().remove(key);
    return true;
}
} // namespace SecretStore
