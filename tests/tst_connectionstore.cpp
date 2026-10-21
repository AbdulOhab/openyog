/* OpenYog — unit tests for qt/ConnectionStore's password handling: the OS
 * keychain first, upstream's base64 INI field as the fallback, and legacy
 * base64 entries moved into the keychain on load.
 *
 * FakeSecretStore.cpp replaces qt/SecretStore.cpp at link time, and
 * XDG_CONFIG_HOME points at a throwaway dir so connections.ini never lands
 * in the real config dir (QStandardPaths test mode would still write under
 * $HOME). */
#include "ConnectionStore.h"
#include "FakeSecretStore.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

namespace {
/* "s3cret" as upstream's INI field stores it */
const char kSecretB64[] = "czNjcmV0";

ConnectionParams mysqlParams(const QString &name)
{
    ConnectionParams p;
    p.name = name;
    p.driverType = SqlDriverType::Mysql;
    p.host = QStringLiteral("db.example");
    p.user = QStringLiteral("root");
    p.password = QStringLiteral("s3cret");
    return p;
}

QString iniPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
        .filePath(QStringLiteral("connections.ini"));
}

QString iniText()
{
    QFile f(iniPath());
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

/* a section the way a pre-keychain build left it */
void writeLegacyIni()
{
    QDir().mkpath(QFileInfo(iniPath()).path());
    QFile f(iniPath());
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray("[legacy]\nhost=db.example\nuser=root\nport=3306\npassword=") + kSecretB64 +
            "\n");
}
} // namespace

class TestConnectionStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void savesPasswordToKeychain();
    void fallsBackToIniWithoutKeychain();
    void migratesLegacyIniPassword();
    void keepsLegacyPasswordWhenKeychainUnavailable();
    void renameMovesSecret();
    void renameRefusedWhenKeychainLocked();
    void removeDeletesSecret();
    void sqliteStoresNoSecret();
    void sshSettingsAndSecretRoundTrip();
    void sshOffDropsItsSecret();
    void removeAndRenameCarryTheSshSecret();

private:
    QTemporaryDir m_configHome;
};

void TestConnectionStore::initTestCase()
{
    QVERIFY(m_configHome.isValid());
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_configHome.path()));
    QVERIFY(iniPath().startsWith(m_configHome.path()));
}

void TestConnectionStore::init()
{
    QFile::remove(iniPath());
    FakeSecretStore::reset(true);
}

void TestConnectionStore::savesPasswordToKeychain()
{
    ConnectionStore::save(mysqlParams(QStringLiteral("prod")));

    QCOMPARE(FakeSecretStore::entries().value(QStringLiteral("connection/prod")),
             QStringLiteral("s3cret"));
    QVERIFY(!iniText().contains(QLatin1String(kSecretB64)));
    QVERIFY(iniText().contains(QStringLiteral("password_in_keychain=1")));

    ConnectionParams back;
    QVERIFY(ConnectionStore::load(QStringLiteral("prod"), &back));
    QCOMPARE(back.password, QStringLiteral("s3cret"));
    QCOMPARE(back.host, QStringLiteral("db.example"));
}

void TestConnectionStore::fallsBackToIniWithoutKeychain()
{
    FakeSecretStore::reset(false);
    ConnectionStore::save(mysqlParams(QStringLiteral("prod")));

    QVERIFY(FakeSecretStore::entries().isEmpty());
    QVERIFY(iniText().contains(QLatin1String(kSecretB64)));
    QVERIFY(iniText().contains(QStringLiteral("password_in_keychain=0")));

    ConnectionParams back;
    QVERIFY(ConnectionStore::load(QStringLiteral("prod"), &back));
    QCOMPARE(back.password, QStringLiteral("s3cret"));
}

void TestConnectionStore::migratesLegacyIniPassword()
{
    writeLegacyIni();

    ConnectionParams back;
    QVERIFY(ConnectionStore::load(QStringLiteral("legacy"), &back));
    QCOMPARE(back.password, QStringLiteral("s3cret"));
    /* moved, not copied */
    QCOMPARE(FakeSecretStore::entries().value(QStringLiteral("connection/legacy")),
             QStringLiteral("s3cret"));
    QVERIFY(!iniText().contains(QLatin1String(kSecretB64)));

    /* and the next load reads it back from the keychain */
    ConnectionParams again;
    QVERIFY(ConnectionStore::load(QStringLiteral("legacy"), &again));
    QCOMPARE(again.password, QStringLiteral("s3cret"));
}

void TestConnectionStore::keepsLegacyPasswordWhenKeychainUnavailable()
{
    FakeSecretStore::reset(false);
    writeLegacyIni();

    ConnectionParams back;
    QVERIFY(ConnectionStore::load(QStringLiteral("legacy"), &back));
    QCOMPARE(back.password, QStringLiteral("s3cret"));
    QVERIFY(iniText().contains(QLatin1String(kSecretB64)));
}

void TestConnectionStore::renameMovesSecret()
{
    ConnectionStore::save(mysqlParams(QStringLiteral("prod")));
    QVERIFY(ConnectionStore::rename(QStringLiteral("prod"), QStringLiteral("prod2")));

    QVERIFY(!FakeSecretStore::entries().contains(QStringLiteral("connection/prod")));
    QCOMPARE(FakeSecretStore::entries().value(QStringLiteral("connection/prod2")),
             QStringLiteral("s3cret"));
    const QStringList names = ConnectionStore::storedNames();
    QVERIFY(names.contains(QStringLiteral("prod2")));
    QVERIFY(!names.contains(QStringLiteral("prod")));

    ConnectionParams back;
    QVERIFY(ConnectionStore::load(QStringLiteral("prod2"), &back));
    QCOMPARE(back.password, QStringLiteral("s3cret"));
}

void TestConnectionStore::renameRefusedWhenKeychainLocked()
{
    ConnectionStore::save(mysqlParams(QStringLiteral("prod")));
    FakeSecretStore::setAvailable(false);

    /* renaming now would store an empty password under the new name and
     * delete the real one */
    QVERIFY(!ConnectionStore::rename(QStringLiteral("prod"), QStringLiteral("prod2")));

    FakeSecretStore::setAvailable(true);
    QCOMPARE(FakeSecretStore::entries().value(QStringLiteral("connection/prod")),
             QStringLiteral("s3cret"));
    QVERIFY(ConnectionStore::storedNames().contains(QStringLiteral("prod")));
    QVERIFY(!ConnectionStore::storedNames().contains(QStringLiteral("prod2")));
}

void TestConnectionStore::removeDeletesSecret()
{
    ConnectionStore::save(mysqlParams(QStringLiteral("prod")));
    ConnectionStore::remove(QStringLiteral("prod"));

    QVERIFY(FakeSecretStore::entries().isEmpty());
    QVERIFY(!ConnectionStore::storedNames().contains(QStringLiteral("prod")));
}

void TestConnectionStore::sqliteStoresNoSecret()
{
    ConnectionParams p;
    p.name = QStringLiteral("local");
    p.driverType = SqlDriverType::Sqlite;
    p.filePath = QStringLiteral("/tmp/x.sqlite");
    ConnectionStore::save(p);

    QVERIFY(FakeSecretStore::entries().isEmpty());
    ConnectionParams back;
    QVERIFY(ConnectionStore::load(QStringLiteral("local"), &back));
    QCOMPARE(back.filePath, QStringLiteral("/tmp/x.sqlite"));
}

namespace {
ConnectionParams sshParams(const QString &name)
{
    ConnectionParams p = mysqlParams(name);
    p.host = QStringLiteral("127.0.0.1"); /* as seen from the SSH host */
    p.useSsh = true;
    p.sshHost = QStringLiteral("bastion.example");
    p.sshPort = 2222;
    p.sshUser = QStringLiteral("tunnel");
    p.sshPassword = QStringLiteral("ssh-s3cret");
    p.sshKeyFile = QStringLiteral("/home/u/.ssh/id_ed25519");
    return p;
}
} // namespace

void TestConnectionStore::sshSettingsAndSecretRoundTrip()
{
    ConnectionStore::save(sshParams(QStringLiteral("viaSsh")));
    /* its own keychain entry, next to the database password's */
    QCOMPARE(FakeSecretStore::entries().value(QStringLiteral("connection/viaSsh/ssh")),
             QStringLiteral("ssh-s3cret"));
    QCOMPARE(FakeSecretStore::entries().value(QStringLiteral("connection/viaSsh")),
             QStringLiteral("s3cret"));
    QVERIFY(!iniText().contains(QStringLiteral("ssh-s3cret")));

    ConnectionParams back;
    QVERIFY(ConnectionStore::load(QStringLiteral("viaSsh"), &back));
    QVERIFY(back.useSsh);
    QCOMPARE(back.sshHost, QStringLiteral("bastion.example"));
    QCOMPARE(back.sshPort, 2222);
    QCOMPARE(back.sshUser, QStringLiteral("tunnel"));
    QCOMPARE(back.sshPassword, QStringLiteral("ssh-s3cret"));
    QCOMPARE(back.sshKeyFile, QStringLiteral("/home/u/.ssh/id_ed25519"));
    QCOMPARE(back.password, QStringLiteral("s3cret"));

    /* no keychain: both fall back to the INI */
    FakeSecretStore::reset(false);
    ConnectionStore::save(sshParams(QStringLiteral("plain")));
    ConnectionParams plain;
    QVERIFY(ConnectionStore::load(QStringLiteral("plain"), &plain));
    QCOMPARE(plain.sshPassword, QStringLiteral("ssh-s3cret"));
    QCOMPARE(plain.password, QStringLiteral("s3cret"));
}

void TestConnectionStore::sshOffDropsItsSecret()
{
    ConnectionStore::save(sshParams(QStringLiteral("p")));
    ConnectionParams off = sshParams(QStringLiteral("p"));
    off.useSsh = false;
    ConnectionStore::save(off);
    QVERIFY(!FakeSecretStore::entries().contains(QStringLiteral("connection/p/ssh")));
    ConnectionParams back;
    QVERIFY(ConnectionStore::load(QStringLiteral("p"), &back));
    QVERIFY(!back.useSsh);
    QVERIFY(back.sshPassword.isEmpty());
}

void TestConnectionStore::removeAndRenameCarryTheSshSecret()
{
    ConnectionStore::save(sshParams(QStringLiteral("a")));
    QVERIFY(ConnectionStore::rename(QStringLiteral("a"), QStringLiteral("b")));
    QVERIFY(!FakeSecretStore::entries().contains(QStringLiteral("connection/a/ssh")));
    QCOMPARE(FakeSecretStore::entries().value(QStringLiteral("connection/b/ssh")),
             QStringLiteral("ssh-s3cret"));

    ConnectionStore::remove(QStringLiteral("b"));
    QVERIFY(FakeSecretStore::entries().isEmpty());
}

QTEST_GUILESS_MAIN(TestConnectionStore)
#include "tst_connectionstore.moc"
