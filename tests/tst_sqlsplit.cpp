/* OpenYog — unit tests for qt/SqlSplit (statement splitting on the current
 * terminator, DELIMITER-aware).
 *
 * knownGaps() pins down inputs the splitter does NOT handle yet: each row
 * states the correct result and is marked QEXPECT_FAIL, so the suite stays
 * green today and turns red (XPASS) the moment one gets fixed — delete that
 * row's expectation then. */
#include "SqlSplit.h"

#include <QTest>

class TestSqlSplit : public QObject
{
    Q_OBJECT

private slots:
    void split_data();
    void split();
    void statementAt_data();
    void statementAt();
    void knownGaps_data();
    void knownGaps();
};

void TestSqlSplit::split_data()
{
    QTest::addColumn<QString>("sql");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("single, no terminator") << "SELECT 1" << QStringList{"SELECT 1"};
    QTest::newRow("two statements") << "SELECT 1; SELECT 2;" << QStringList{"SELECT 1", "SELECT 2"};
    QTest::newRow("empty statements dropped") << ";;SELECT 1;;  ;" << QStringList{"SELECT 1"};
    QTest::newRow("blank input") << "  \n\t " << QStringList{};
    QTest::newRow("statements are trimmed") << "\n  SELECT 1  ;\n\n  SELECT 2\n"
                                            << QStringList{"SELECT 1", "SELECT 2"};
    QTest::newRow("terminator inside string")
        << "SELECT 'a;b'; SELECT 2" << QStringList{"SELECT 'a;b'", "SELECT 2"};
    QTest::newRow("doubled quote inside string")
        << "SELECT 'it''s; fine'; SELECT 2" << QStringList{"SELECT 'it''s; fine'", "SELECT 2"};
    QTest::newRow("DELIMITER keeps a compound body whole")
        << "DELIMITER $$\nCREATE PROCEDURE p()\nBEGIN\n  SELECT 1;\n  SELECT 2;\nEND$$\n"
           "DELIMITER ;\nSELECT 3;"
        << QStringList{"CREATE PROCEDURE p()\nBEGIN\n  SELECT 1;\n  SELECT 2;\nEND", "SELECT 3"};
    QTest::newRow("DELIMITER is case-insensitive")
        << "delimiter //\nSELECT 1;SELECT 2//\ndelimiter ;\nSELECT 3;"
        << QStringList{"SELECT 1;SELECT 2", "SELECT 3"};
    QTest::newRow("DELIMITER line with CRLF")
        << "DELIMITER $$\r\nSELECT 1;$$\r\nDELIMITER ;\r\nSELECT 2;"
        << QStringList{"SELECT 1;", "SELECT 2"};
    QTest::newRow("DELIMITER inside a string is data")
        << "SELECT 'x\nDELIMITER $$\n'; SELECT 2"
        << QStringList{"SELECT 'x\nDELIMITER $$\n'", "SELECT 2"};
}

void TestSqlSplit::split()
{
    QFETCH(QString, sql);
    QFETCH(QStringList, expected);
    QCOMPARE(splitStatements(sql), expected);
}

void TestSqlSplit::statementAt_data()
{
    QTest::addColumn<QString>("sql");
    QTest::addColumn<int>("pos");
    QTest::addColumn<QString>("expected");

    const QString two = QStringLiteral("SELECT 1; SELECT 2;");
    QTest::newRow("inside first") << two << 3 << "SELECT 1";
    QTest::newRow("inside second") << two << 13 << "SELECT 2";
    QTest::newRow("right after first terminator") << two << 9 << "SELECT 1";
    QTest::newRow("unterminated tail") << "SELECT 1; SELECT 2" << 15 << "SELECT 2";
    QTest::newRow("blank tail falls back to last") << "SELECT 1;\n\n" << 11 << "SELECT 1";
    QTest::newRow("empty input") << "" << 0 << "";
    QTest::newRow("DELIMITER block")
        << "DELIMITER $$\nCREATE PROCEDURE p()\nBEGIN\n  SELECT 1;\nEND$$\nDELIMITER ;\n"
        << 30 << "CREATE PROCEDURE p()\nBEGIN\n  SELECT 1;\nEND";
}

void TestSqlSplit::statementAt()
{
    QFETCH(QString, sql);
    QFETCH(int, pos);
    QFETCH(QString, expected);
    QCOMPARE(::statementAt(sql, pos), expected);
}

void TestSqlSplit::knownGaps_data()
{
    QTest::addColumn<QString>("sql");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("terminator in -- comment")
        << "-- a;b\nSELECT 1" << QStringList{"-- a;b\nSELECT 1"};
    QTest::newRow("apostrophe in -- comment")
        << "-- don't\nSELECT 1; SELECT 2" << QStringList{"-- don't\nSELECT 1", "SELECT 2"};
    QTest::newRow("terminator in /* */ comment")
        << "/* a; b */ SELECT 1" << QStringList{"/* a; b */ SELECT 1"};
    QTest::newRow("terminator in double-quoted string")
        << "SELECT \"a;b\"; SELECT 2" << QStringList{"SELECT \"a;b\"", "SELECT 2"};
    QTest::newRow("terminator in backtick identifier")
        << "SELECT `a;b` FROM t; SELECT 2" << QStringList{"SELECT `a;b` FROM t", "SELECT 2"};
    QTest::newRow("PostgreSQL dollar-quoted body")
        << "CREATE FUNCTION f() RETURNS int AS $$ BEGIN RETURN 1; END; $$ LANGUAGE plpgsql;"
           " SELECT 2"
        << QStringList{"CREATE FUNCTION f() RETURNS int AS $$ BEGIN RETURN 1; END; $$ LANGUAGE "
                       "plpgsql",
                       "SELECT 2"};
}

void TestSqlSplit::knownGaps()
{
    QFETCH(QString, sql);
    QFETCH(QStringList, expected);
    QEXPECT_FAIL("", "splitter only tracks single-quoted strings (see SqlSplit.h)", Abort);
    QCOMPARE(splitStatements(sql), expected);
}

QTEST_GUILESS_MAIN(TestSqlSplit)
#include "tst_sqlsplit.moc"
