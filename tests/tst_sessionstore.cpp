/* OpenYog — unit tests for qt/SessionStore (query tabs and open connections
 * remembered between runs). XDG_DATA_HOME points at a throwaway dir, so
 * session.json never lands in the real data dir. */
#include "SessionStore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using SessionStore::EditorState;

class TestSessionStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void editorsRoundTrip();
    void emptyTabsAreNotKept();
    void connectionsAreSeparate();
    void openConnections();
    void restoreOnStartupDefaultsOn();
    void corruptFileIsTreatedAsEmpty();

private:
    QTemporaryDir m_dataHome;
};

void TestSessionStore::initTestCase()
{
    QVERIFY(m_dataHome.isValid());
    qputenv("XDG_DATA_HOME", QFile::encodeName(m_dataHome.path()));
}

void TestSessionStore::editorsRoundTrip()
{
    const QList<EditorState> tabs{{"Query 1", "SELECT 1;\nSELECT 'বাংলা';", 12},
                                  {"Totals", "SELECT COUNT(*) FROM t", 3}};
    SessionStore::setEditors(QStringLiteral("prod"), tabs);
    const QList<EditorState> back = SessionStore::editors(QStringLiteral("prod"));
    QCOMPARE(back.size(), 2);
    for(int i = 0; i < 2; ++i) {
        QCOMPARE(back[i].title, tabs[i].title);
        QCOMPARE(back[i].text, tabs[i].text);
        QCOMPARE(back[i].cursor, tabs[i].cursor);
    }
}

void TestSessionStore::emptyTabsAreNotKept()
{
    SessionStore::setEditors(QStringLiteral("p"),
                             {{"Query 1", "  \n ", 0}, {"Query 2", "SELECT 2", 0}});
    QCOMPARE(SessionStore::editors(QStringLiteral("p")).size(), 1);
    /* nothing left: the connection's entry goes */
    SessionStore::setEditors(QStringLiteral("p"), {{"Query 1", "", 0}});
    QVERIFY(SessionStore::editors(QStringLiteral("p")).isEmpty());
}

void TestSessionStore::connectionsAreSeparate()
{
    SessionStore::setEditors(QStringLiteral("a"), {{"Query 1", "SELECT 'a'", 0}});
    SessionStore::setEditors(QStringLiteral("b"), {{"Query 1", "SELECT 'b'", 0}});
    QCOMPARE(SessionStore::editors(QStringLiteral("a")).value(0).text,
             QStringLiteral("SELECT 'a'"));
    QCOMPARE(SessionStore::editors(QStringLiteral("b")).value(0).text,
             QStringLiteral("SELECT 'b'"));
    QVERIFY(SessionStore::editors(QStringLiteral("never-saved")).isEmpty());
}

void TestSessionStore::openConnections()
{
    SessionStore::setOpenConnections({QStringLiteral("prod"), QStringLiteral("local")});
    QCOMPARE(SessionStore::openConnections(), (QStringList{"prod", "local"}));
    /* the editors written before are still there */
    QCOMPARE(SessionStore::editors(QStringLiteral("prod")).size(), 2);
}

void TestSessionStore::restoreOnStartupDefaultsOn()
{
    QVERIFY(SessionStore::restoreOnStartup());
    SessionStore::setRestoreOnStartup(false);
    QVERIFY(!SessionStore::restoreOnStartup());
    SessionStore::setRestoreOnStartup(true);
    QVERIFY(SessionStore::restoreOnStartup());
}

void TestSessionStore::corruptFileIsTreatedAsEmpty()
{
    QFile f(m_dataHome.filePath(QStringLiteral("tst_sessionstore/session.json")));
    QVERIFY(f.exists());
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{ not json");
    f.close();
    QVERIFY(SessionStore::editors(QStringLiteral("prod")).isEmpty());
    QVERIFY(SessionStore::openConnections().isEmpty());
    QVERIFY(SessionStore::restoreOnStartup());
    /* and the next save writes a good file again */
    SessionStore::setOpenConnections({QStringLiteral("x")});
    QCOMPARE(SessionStore::openConnections(), QStringList{"x"});
}

QTEST_GUILESS_MAIN(TestSessionStore)
#include "tst_sessionstore.moc"
