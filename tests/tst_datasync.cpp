/* OpenYog — unit tests for qt/DataSync on real SQLite databases: what
 * differs between two tables' rows, and a script that, run on the target,
 * leaves nothing to sync. */
#include "DataSync.h"
#include "db/SqliteConnection.h"

#include <QTemporaryDir>
#include <QTest>

#include <sqlite3.h>

using namespace DataSync;

namespace {

void exec(const QString &file, const QByteArray &sql)
{
    sqlite3 *db = nullptr;
    QCOMPARE(sqlite3_open(file.toUtf8().constData(), &db), SQLITE_OK);
    char *error = nullptr;
    const int rc = sqlite3_exec(db, sql.constData(), nullptr, nullptr, &error);
    const QString message = error ? QString::fromUtf8(error) : QString();
    sqlite3_free(error);
    sqlite3_close(db);
    QVERIFY2(rc == SQLITE_OK, qPrintable(message));
}

SqliteConnection *open(const QString &file)
{
    sqlite3 *db = nullptr;
    sqlite3_open(file.toUtf8().constData(), &db);
    return new SqliteConnection(db);
}

const char *kSchema = R"(
CREATE TABLE emp_dept (id INTEGER PRIMARY KEY, name TEXT NOT NULL);
CREATE TABLE employees (id INTEGER PRIMARY KEY, name TEXT, dept_id INTEGER REFERENCES emp_dept(id),
                        photo BLOB, note TEXT);
CREATE TABLE pairs (a INTEGER, b INTEGER, val TEXT, PRIMARY KEY (a, b));
CREATE TABLE nokey (x TEXT);
)";

} // namespace

class TestDataSync : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_source = m_dir.filePath(QStringLiteral("source-%1.sqlite").arg(++m_n));
        m_target = m_dir.filePath(QStringLiteral("target-%1.sqlite").arg(m_n));
        exec(m_source, QByteArray(kSchema) + R"(
            CREATE TABLE only_in_source (id INTEGER PRIMARY KEY);
            INSERT INTO emp_dept VALUES (1, 'Engineering'), (2, 'Sales'), (4, 'Support');
            INSERT INTO employees VALUES
              (1, 'Alice', 1, NULL, 'same'),
              (2, 'O''Brien;' || char(10) || 'রহিম', 2, x'89504E4700FF', ''),
              (3, 'Dana', 4, NULL, NULL);
            INSERT INTO pairs VALUES (1, 1, 'a'), (1, 2, 'b'), (3, 3, 'c');
            INSERT INTO nokey VALUES ('x');
        )");
        exec(m_target, QByteArray(kSchema) + R"(
            ALTER TABLE employees ADD COLUMN legacy TEXT;
            INSERT INTO emp_dept VALUES (1, 'Engineering'), (2, 'Marketing'), (3, 'Legacy');
            INSERT INTO employees (id, name, dept_id, photo, note, legacy) VALUES
              (1, 'Alice', 1, NULL, 'same', 'kept as is'),
              (2, 'O''Brien', 2, x'00', NULL, NULL),
              (9, 'Gone', 3, NULL, NULL, NULL);
            INSERT INTO pairs VALUES (1, 1, 'a'), (1, 2, 'B'), (2, 1, 'z');
        )");
    }

    void tablesInForeignKeyOrder()
    {
        std::unique_ptr<SqliteConnection> s(open(m_source)), t(open(m_target));
        const QStringList tables = commonTables(s.get(), "main", t.get(), "main");
        QCOMPARE(tables.size(), 4); /* not only_in_source */
        QVERIFY(tables.indexOf("emp_dept") < tables.indexOf("employees"));
    }

    void whatDiffers()
    {
        std::unique_ptr<SqliteConnection> s(open(m_source)), t(open(m_target));
        const TableResult dept = compareTable(s.get(), "main", t.get(), "main", "emp_dept");
        QCOMPARE(dept.key, QStringList{"id"});
        QCOMPARE(dept.same, 1);
        QCOMPARE(dept.updates, 1);
        QCOMPARE(dept.inserts, 1);
        QCOMPARE(dept.deletes, 1);

        const TableResult emp = compareTable(s.get(), "main", t.get(), "main", "employees");
        QCOMPARE(emp.ignored, QStringList{"legacy"});
        QCOMPARE(emp.columns, QStringList({"id", "name", "dept_id", "photo", "note"}));
        QCOMPARE(emp.same, 1); /* the target's own column doesn't count */
        QCOMPARE(emp.updates, 1);
        QCOMPARE(emp.inserts, 1);
        QCOMPARE(emp.deletes, 1);

        const TableResult pairs = compareTable(s.get(), "main", t.get(), "main", "pairs");
        QCOMPARE(pairs.key, QStringList({"a", "b"}));
        QCOMPARE(pairs.same + pairs.updates + pairs.inserts + pairs.deletes, 4);
        QCOMPARE(pairs.updates, 1);

        const TableResult nokey = compareTable(s.get(), "main", t.get(), "main", "nokey");
        QVERIFY(nokey.skipped.contains("primary key"));
    }

    void scriptLeavesNothingToSync()
    {
        QByteArray script;
        {
            std::unique_ptr<SqliteConnection> s(open(m_source)), t(open(m_target));
            QList<Pick> picks;
            for(const QString &table : commonTables(s.get(), "main", t.get(), "main"))
                picks.append({table, true, true, true});
            QString error;
            QVERIFY2(writeScript(
                         s.get(), "main", t.get(), "main", picks,
                         [&](const QByteArray &part) { script += part; }, {}, &error),
                     qPrintable(error));
        }
        /* children's rows out before their parents', parents' in first */
        QVERIFY(script.indexOf("DELETE FROM \"main\".\"employees\"") <
                script.indexOf("DELETE FROM \"main\".\"emp_dept\""));
        QVERIFY(script.indexOf("INSERT INTO \"main\".\"emp_dept\"") <
                script.indexOf("INSERT INTO \"main\".\"employees\""));
        QVERIFY(script.startsWith("-- Data sync"));
        QVERIFY(script.contains("PRAGMA foreign_keys = OFF;\nBEGIN;\n"));
        QVERIFY(script.contains("x'89504e4700ff'"));

        exec(m_target, script);

        std::unique_ptr<SqliteConnection> s(open(m_source)), t(open(m_target));
        for(const QString &table : commonTables(s.get(), "main", t.get(), "main")) {
            const TableResult r = compareTable(s.get(), "main", t.get(), "main", table);
            if(table == "nokey")
                continue;
            QVERIFY2(r.inserts + r.updates + r.deletes == 0, qPrintable(table));
        }
        /* the bytes, the quote, the newline and the Bengali survived */
        DbResultSet rs;
        QVERIFY(
            t->query("SELECT hex(photo), name, note IS NULL, legacy FROM employees WHERE id = 2",
                     &rs, nullptr));
        QCOMPARE(rs.rows.value(0).value(0), QStringLiteral("89504E4700FF"));
        QCOMPARE(rs.rows.value(0).value(1), QStringLiteral("O'Brien;\nরহিম"));
        QCOMPARE(rs.rows.value(0).value(2), QStringLiteral("0")); /* '' is not NULL */
        QCOMPARE(rs.rows.value(0).value(3), QStringLiteral("NULL"));
    }

    void deletesLeftOutUnlessPicked()
    {
        QByteArray script;
        {
            std::unique_ptr<SqliteConnection> s(open(m_source)), t(open(m_target));
            QList<Pick> picks;
            for(const QString &table : commonTables(s.get(), "main", t.get(), "main"))
                picks.append({table}); /* inserts and updates only */
            QVERIFY(writeScript(s.get(), "main", t.get(), "main", picks,
                                [&](const QByteArray &part) { script += part; }));
        }
        QVERIFY(!script.contains("DELETE"));
        exec(m_target, script);
        std::unique_ptr<SqliteConnection> s(open(m_source)), t(open(m_target));
        const TableResult dept = compareTable(s.get(), "main", t.get(), "main", "emp_dept");
        QCOMPARE(dept.inserts + dept.updates, 0);
        QCOMPARE(dept.deletes, 1); /* 'Legacy' is still there */
    }

    void cancelStops()
    {
        std::unique_ptr<SqliteConnection> s(open(m_source)), t(open(m_target));
        QString error;
        QVERIFY(!writeScript(
            s.get(), "main", t.get(), "main", {{"emp_dept"}}, [](const QByteArray &) {},
            [] { return true; }, &error));
        QVERIFY(error.contains("cancelled"));
    }

private:
    QTemporaryDir m_dir;
    QString m_source, m_target;
    int m_n = 0;
};

QTEST_GUILESS_MAIN(TestDataSync)
#include "tst_datasync.moc"
