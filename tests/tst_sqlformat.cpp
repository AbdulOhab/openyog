/* OpenYog — unit tests for qt/SqlFormat (the F12 SQL pretty-printer).
 *
 * The golden rows pin today's layout so a formatter change shows up as a
 * reviewable diff; the property tests (literals untouched, idempotent) are
 * the ones that must never regress. */
#include "SqlFormat.h"

#include <QTest>

class TestSqlFormat : public QObject
{
    Q_OBJECT

private slots:
    void golden_data();
    void golden();
    void preservesLiterals_data();
    void preservesLiterals();
    void idempotent_data();
    void idempotent();
};

void TestSqlFormat::golden_data()
{
    QTest::addColumn<QString>("sql");
    QTest::addColumn<QString>("expected");

    QTest::newRow("select list") << "select a,b from t where x=1"
                                 << "SELECT a,\n  b\nFROM t\nWHERE x = 1;";
    QTest::newRow("two statements") << "select 1; select 2" << "SELECT 1;\n\nSELECT 2;";
    QTest::newRow("join + subquery")
        << "select * from a join b on a.id=b.id where a.x in (select y from c) order by 1 limit 5"
        << "SELECT *\nFROM a\nJOIN b\nON a.id = b.id\nWHERE a.x IN (SELECT y FROM c)\n"
           "ORDER BY 1\nLIMIT 5;";
    QTest::newRow("insert") << "insert into t (a,b) values (1,'x')"
                            << "INSERT INTO t (a, b)\nVALUES (1, 'x');";
    QTest::newRow("update") << "update t set a=1 where b=2" << "UPDATE t\nSET a = 1\nWHERE b = 2;";
}

void TestSqlFormat::golden()
{
    QFETCH(QString, sql);
    QFETCH(QString, expected);
    QCOMPARE(SqlFormat::pretty(sql), expected);
}

void TestSqlFormat::preservesLiterals_data()
{
    QTest::addColumn<QString>("sql");
    QTest::addColumn<QString>("mustContain");

    QTest::newRow("keywords in a string") << "select 'from where' as s from t" << "'from where'";
    QTest::newRow("keyword as backtick identifier") << "select `select` from t" << "`select`";
    QTest::newRow("line comment") << "-- keep me\nselect 1" << "-- keep me";
    QTest::newRow("block comment") << "select /* from x */ 1" << "/* from x */";
    QTest::newRow("mixed-case string") << "select 'MiXeD case' from t" << "'MiXeD case'";
}

void TestSqlFormat::preservesLiterals()
{
    QFETCH(QString, sql);
    QFETCH(QString, mustContain);
    QVERIFY2(SqlFormat::pretty(sql).contains(mustContain), qPrintable(SqlFormat::pretty(sql)));
}

void TestSqlFormat::idempotent_data()
{
    QTest::addColumn<QString>("sql");

    QTest::newRow("select") << "select a,b from t where x=1 and y<>'z' order by a desc";
    QTest::newRow("join")
        << "select * from a left join b on a.id=b.id group by a.k having count(*)>1";
    QTest::newRow("insert") << "insert into t (a,b) values (1,'x'),(2,'y')";
    QTest::newRow("update") << "update t set a=1, b='q' where c is null";
    QTest::newRow("delete") << "delete from t where id in (1,2,3)";
}

void TestSqlFormat::idempotent()
{
    QFETCH(QString, sql);
    const QString once = SqlFormat::pretty(sql);
    QCOMPARE(SqlFormat::pretty(once), once);
}

QTEST_GUILESS_MAIN(TestSqlFormat)
#include "tst_sqlformat.moc"
