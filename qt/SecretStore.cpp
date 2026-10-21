#include "SecretStore.h"

#ifdef OPENYOG_HAVE_KEYCHAIN

#include <QEventLoop>

#include <qt6keychain/keychain.h>

namespace {
/* the "service" every entry is filed under in the keyring UI (Seahorse,
 * KWalletManager, Credential Manager …) */
const QString kService = QStringLiteral("OpenYog");

QKeychain::Error runJob(QKeychain::Job &job)
{
    job.setAutoDelete(false);
    job.setInsecureFallback(false); /* plaintext QSettings is what we're replacing */
    QEventLoop loop;
    bool done = false;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, [&] {
        done = true;
        loop.quit();
    });
    job.start();
    /* guard against a backend that finishes inside start() */
    if(!done)
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    return job.error();
}
} // namespace

namespace SecretStore {
bool read(const QString &key, QString *secret)
{
    QKeychain::ReadPasswordJob job(kService);
    job.setKey(key);
    if(runJob(job) != QKeychain::NoError)
        return false;
    *secret = job.textData();
    return true;
}

bool write(const QString &key, const QString &secret)
{
    QKeychain::WritePasswordJob job(kService);
    job.setKey(key);
    job.setTextData(secret);
    return runJob(job) == QKeychain::NoError;
}

bool remove(const QString &key)
{
    QKeychain::DeletePasswordJob job(kService);
    job.setKey(key);
    const QKeychain::Error e = runJob(job);
    return e == QKeychain::NoError || e == QKeychain::EntryNotFound;
}
} // namespace SecretStore

#else /* built without QtKeychain */

namespace SecretStore {
bool read(const QString &, QString *)
{
    return false;
}

bool write(const QString &, const QString &)
{
    return false;
}

bool remove(const QString &)
{
    return false;
}
} // namespace SecretStore

#endif
