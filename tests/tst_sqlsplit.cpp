/* OpenYog — unit tests for qt/SqlSplit (statement splitting on the current
 * terminator: DELIMITER-aware, skipping quoted text and comments the way
 * each dialect reads them). */
#include "SqlSplit.h"

#include <QTest>

class TestSqlSplit : public QObject
{
    Q_OBJECT

private slots:
    void split_data();
    void split();
    void dialects_data();
    void dialects();
    void statementAt_data();
    void statementAt();
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
    QTest::newRow("unterminated string runs to the end")
        << "SELECT 'abc; SELECT 2" << QStringList{"SELECT 'abc; SELECT 2"};

    /* quoted text */
    QTest::newRow("terminator inside string")
        << "SELECT 'a;b'; SELECT 2" << QStringList{"SELECT 'a;b'", "SELECT 2"};
    QTest::newRow("doubled quote inside string")
        << "SELECT 'it''s; fine'; SELECT 2" << QStringList{"SELECT 'it''s; fine'", "SELECT 2"};
    QTest::newRow("terminator in double-quoted string")
        << "SELECT \"a;b\"; SELECT 2" << QStringList{"SELECT \"a;b\"", "SELECT 2"};
    QTest::newRow("terminator in backtick identifier")
        << "SELECT `a;b` FROM t; SELECT 2" << QStringList{"SELECT `a;b` FROM t", "SELECT 2"};
    QTest::newRow("doubled backtick")
        << "SELECT `a``;b` FROM t; SELECT 2" << QStringList{"SELECT `a``;b` FROM t", "SELECT 2"};

    /* comments */
    QTest::newRow("terminator in -- comment")
        << "-- a;b\nSELECT 1" << QStringList{"-- a;b\nSELECT 1"};
    QTest::newRow("apostrophe in -- comment")
        << "-- don't\nSELECT 1; SELECT 2" << QStringList{"-- don't\nSELECT 1", "SELECT 2"};
    QTest::newRow("terminator in block comment")
        << "/* a; b */ SELECT 1" << QStringList{"/* a; b */ SELECT 1"};
    QTest::newRow("quote in block comment")
        << "/* it's */ SELECT 1; SELECT 2" << QStringList{"/* it's */ SELECT 1", "SELECT 2"};
    QTest::newRow("comments stay with their statement")
        << "-- header\nSELECT 1; SELECT 2 -- tail"
        << QStringList{"-- header\nSELECT 1", "SELECT 2 -- tail"};
    QTest::newRow("comment-only tail is not a statement") << "SELECT 1;\n-- trailing\n"
                                                          << QStringList{"SELECT 1"};
    QTest::newRow("comment-only statement dropped")
        << "SELECT 1; /* x */; SELECT 2" << QStringList{"SELECT 1", "SELECT 2"};
    QTest::newRow("comment-only input") << "/* only */\n-- comments" << QStringList{};
    QTest::newRow("unterminated block comment") << "SELECT 1; /* open" << QStringList{"SELECT 1"};

    /* DELIMITER */
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
    QTest::newRow("DELIMITER inside a block comment is ignored")
        << "/*\nDELIMITER $$\n*/SELECT 1; SELECT 2"
        << QStringList{"/*\nDELIMITER $$\n*/SELECT 1", "SELECT 2"};
    QTest::newRow("DELIMITER after a -- comment line")
        << "-- c\nDELIMITER $$\nSELECT 1;$$" << QStringList{"SELECT 1;"};
}

void TestSqlSplit::split()
{
    QFETCH(QString, sql);
    QFETCH(QStringList, expected);
    QCOMPARE(splitStatements(sql), expected);
}

void TestSqlSplit::dialects_data()
{
    QTest::addColumn<QString>("sql");
    QTest::addColumn<SqlDriverType>("driver");
    QTest::addColumn<QStringList>("expected");

    const auto my = SqlDriverType::Mysql;
    const auto pg = SqlDriverType::Postgres;
    const auto lite = SqlDriverType::Sqlite;

    /* backslash escapes: MySQL yes, PostgreSQL only in E'…', SQLite never */
    QTest::newRow("MySQL backslash-escaped quote")
        << "SELECT 'it\\'s; x'; SELECT 2" << my << QStringList{"SELECT 'it\\'s; x'", "SELECT 2"};
    QTest::newRow("PostgreSQL backslash is literal")
        << "SELECT 'C:\\'; SELECT 2" << pg << QStringList{"SELECT 'C:\\'", "SELECT 2"};
    QTest::newRow("SQLite backslash is literal")
        << "SELECT 'C:\\'; SELECT 2" << lite << QStringList{"SELECT 'C:\\'", "SELECT 2"};
    QTest::newRow("PostgreSQL E-string escapes")
        << "SELECT E'it\\'s; x'; SELECT 2" << pg << QStringList{"SELECT E'it\\'s; x'", "SELECT 2"};

    /* line comments */
    QTest::newRow("MySQL # comment") << "# a;b\nSELECT 1" << my << QStringList{"# a;b\nSELECT 1"};
    QTest::newRow("PostgreSQL # is an operator")
        << "SELECT 1 # 2; SELECT 3" << pg << QStringList{"SELECT 1 # 2", "SELECT 3"};
    QTest::newRow("MySQL 1--1 is arithmetic")
        << "SELECT 1--1; SELECT 2" << my << QStringList{"SELECT 1--1", "SELECT 2"};
    QTest::newRow("PostgreSQL -- needs no space")
        << "SELECT 1 --x;y\n; SELECT 2" << pg << QStringList{"SELECT 1 --x;y", "SELECT 2"};

    /* PostgreSQL dollar quoting */
    QTest::newRow("dollar-quoted body")
        << "CREATE FUNCTION f() RETURNS int AS $$ BEGIN RETURN 1; END; $$ LANGUAGE plpgsql;"
           " SELECT 2"
        << pg
        << QStringList{"CREATE FUNCTION f() RETURNS int AS $$ BEGIN RETURN 1; END; $$ LANGUAGE "
                       "plpgsql",
                       "SELECT 2"};
    QTest::newRow("tagged dollar quote with $$ inside")
        << "DO $fn$ BEGIN RAISE NOTICE '$$;'; END $fn$; SELECT 2" << pg
        << QStringList{"DO $fn$ BEGIN RAISE NOTICE '$$;'; END $fn$", "SELECT 2"};
    QTest::newRow("$1 parameter is not a quote")
        << "SELECT $1; SELECT $2" << pg << QStringList{"SELECT $1", "SELECT $2"};
    QTest::newRow("$ inside an identifier is not a quote")
        << "SELECT a$b$ FROM t; SELECT 2" << pg << QStringList{"SELECT a$b$ FROM t", "SELECT 2"};
    QTest::newRow("DELIMITER $$ wins over dollar quoting")
        << "DELIMITER $$\nCREATE FUNCTION f() AS $body$ BEGIN RETURN 1; END; $body$\n$$\n"
           "DELIMITER ;\nSELECT 2;"
        << pg
        << QStringList{"CREATE FUNCTION f() AS $body$ BEGIN RETURN 1; END; $body$", "SELECT 2"};
    QTest::newRow("MySQL has no dollar quotes")
        << "SELECT 1 AS $a$; SELECT 2" << my << QStringList{"SELECT 1 AS $a$", "SELECT 2"};
}

void TestSqlSplit::dialects()
{
    QFETCH(QString, sql);
    QFETCH(SqlDriverType, driver);
    QFETCH(QStringList, expected);
    QCOMPARE(splitStatements(sql, driver), expected);
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
    QTest::newRow("trailing comment falls back to last") << "SELECT 1;\n-- note\n"
                                                         << 13 << "SELECT 1";
    QTest::newRow("comment between statements picks the one before")
        << "SELECT 1; /* x */; SELECT 2;" << 12 << "SELECT 1";
    QTest::newRow("empty input") << "" << 0 << "";
    QTest::newRow("terminator in a comment doesn't split")
        << "SELECT 1 -- a;b\n  + 1;" << 20 << "SELECT 1 -- a;b\n  + 1";
    QTest::newRow("DELIMITER block")
        << "DELIMITER $$\nCREATE PROCEDURE p()\nBEGIN\n  SELECT 1;\nEND$$\nDELIMITER ;\n"
        << 30 << "CREATE PROCEDURE p()\nBEGIN\n  SELECT 1;\nEND";
    QTest::newRow("caret on a DELIMITER line picks the one before")
        << "SELECT 1;\nDELIMITER $$\nSELECT 2$$" << 13 << "SELECT 1";
}

void TestSqlSplit::statementAt()
{
    QFETCH(QString, sql);
    QFETCH(int, pos);
    QFETCH(QString, expected);
    QCOMPARE(::statementAt(sql, pos), expected);
}

QTEST_GUILESS_MAIN(TestSqlSplit)
#include "tst_sqlsplit.moc"
