/* OpenYog — unit tests for qt/SchemaSql (Create/Alter View, Procedure,
 * Function, Trigger, Event editor text).
 *
 * The editorText() checks run the generated text back through
 * splitStatements(), because that is what the query tab does on Execute:
 * the DELIMITER wrapping only matters if the splitter really hands the
 * server one DROP and one intact CREATE. */
#include "SchemaSql.h"
#include "SqlSplit.h"

#include <QTest>

class TestSchemaSql : public QObject
{
    Q_OBJECT

private slots:
    void stripDefiner_data();
    void stripDefiner();
    void createTemplateQuoting();
    void createTemplateEventIsMysqlOnly();
    void showCreateColumn();
    void viewAlterBecomesCreateOrReplace();
    void mysqlRoutineAlterSplitsIntoDropAndCreate_data();
    void mysqlRoutineAlterSplitsIntoDropAndCreate();
    void mysqlCreateHasNoDrop();
    void postgresFunctionAlterIsOneStatement();
    void postgresCreateTemplatesStayWhole_data();
    void postgresCreateTemplatesStayWhole();
    void postgresTriggerAlterDropsOnItsTable_data();
    void postgresTriggerAlterDropsOnItsTable();
};

void TestSchemaSql::stripDefiner_data()
{
    QTest::addColumn<QString>("ddl");
    QTest::addColumn<QString>("expected");

    QTest::newRow("backtick-quoted")
        << "CREATE DEFINER=`root`@`localhost` PROCEDURE `p`() BEGIN END"
        << "CREATE PROCEDURE `p`() BEGIN END";
    QTest::newRow("unquoted") << "CREATE DEFINER=root@localhost VIEW v AS SELECT 1"
                              << "CREATE VIEW v AS SELECT 1";
    QTest::newRow("spaces around =") << "CREATE DEFINER = `u`@`%` TRIGGER t BEFORE INSERT ON x"
                                     << "CREATE TRIGGER t BEFORE INSERT ON x";
    QTest::newRow("no definer is a no-op") << "CREATE OR REPLACE FUNCTION f() RETURNS int"
                                           << "CREATE OR REPLACE FUNCTION f() RETURNS int";
}

void TestSchemaSql::stripDefiner()
{
    QFETCH(QString, ddl);
    QFETCH(QString, expected);
    QCOMPARE(SchemaSql::stripDefiner(ddl), expected);
}

void TestSchemaSql::createTemplateQuoting()
{
    QVERIFY(SchemaSql::createTemplate(QStringLiteral("VIEW"), QStringLiteral("shop"))
                .startsWith(QStringLiteral("CREATE VIEW `shop`.`new_view`")));
    QVERIFY(SchemaSql::createTemplate(QStringLiteral("VIEW"), QStringLiteral("shop"),
                                      SqlDriverType::Postgres)
                .startsWith(QStringLiteral("CREATE VIEW \"shop\".\"new_view\"")));
    /* the quote character inside a name is doubled, not left to break out */
    QVERIFY(SchemaSql::createTemplate(QStringLiteral("PROCEDURE"), QStringLiteral("a`b"))
                .contains(QStringLiteral("`a``b`.`new_proc`")));
    QVERIFY(SchemaSql::createTemplate(QStringLiteral("FUNCTION"), QStringLiteral("a\"b"),
                                      SqlDriverType::Postgres)
                .contains(QStringLiteral("\"a\"\"b\".\"new_func\"")));
}

void TestSchemaSql::createTemplateEventIsMysqlOnly()
{
    QVERIFY(!SchemaSql::createTemplate(QStringLiteral("EVENT"), QStringLiteral("d")).isEmpty());
    QVERIFY(SchemaSql::createTemplate(QStringLiteral("EVENT"), QStringLiteral("d"),
                                      SqlDriverType::Postgres)
                .isEmpty());
    QVERIFY(SchemaSql::createTemplate(QStringLiteral("NOPE"), QStringLiteral("d")).isEmpty());
}

void TestSchemaSql::showCreateColumn()
{
    QCOMPARE(SchemaSql::showCreateColumn(QStringLiteral("VIEW")), 1);
    QCOMPARE(SchemaSql::showCreateColumn(QStringLiteral("PROCEDURE")), 2);
    QCOMPARE(SchemaSql::showCreateColumn(QStringLiteral("FUNCTION")), 2);
    QCOMPARE(SchemaSql::showCreateColumn(QStringLiteral("TRIGGER")), 2);
    QCOMPARE(SchemaSql::showCreateColumn(QStringLiteral("EVENT")), 3);
}

void TestSchemaSql::viewAlterBecomesCreateOrReplace()
{
    QCOMPARE(SchemaSql::editorText(QStringLiteral("VIEW"), QStringLiteral("d"), QStringLiteral("v"),
                                   QStringLiteral("CREATE ALGORITHM=UNDEFINED VIEW `d`.`v` AS "
                                                  "SELECT 1;"),
                                   false),
             QStringLiteral("CREATE OR REPLACE ALGORITHM=UNDEFINED VIEW `d`.`v` AS SELECT 1;\n"));
    /* already OR REPLACE: not doubled */
    QCOMPARE(SchemaSql::editorText(QStringLiteral("VIEW"), QStringLiteral("d"), QStringLiteral("v"),
                                   QStringLiteral("create or replace view v as select 1"), false),
             QStringLiteral("CREATE OR REPLACE view v as select 1;\n"));
}

void TestSchemaSql::mysqlRoutineAlterSplitsIntoDropAndCreate_data()
{
    QTest::addColumn<QString>("objType");
    QTest::addColumn<SqlDriverType>("driver");

    QTest::newRow("MySQL PROCEDURE") << "PROCEDURE" << SqlDriverType::Mysql;
    QTest::newRow("MySQL FUNCTION") << "FUNCTION" << SqlDriverType::Mysql;
    QTest::newRow("MySQL TRIGGER") << "TRIGGER" << SqlDriverType::Mysql;
    QTest::newRow("MySQL EVENT") << "EVENT" << SqlDriverType::Mysql;
    QTest::newRow("SQLite TRIGGER") << "TRIGGER" << SqlDriverType::Sqlite;
}

void TestSchemaSql::mysqlRoutineAlterSplitsIntoDropAndCreate()
{
    QFETCH(QString, objType);
    QFETCH(SqlDriverType, driver);

    const QString body = QStringLiteral("CREATE %1 `d`.`o`()\nBEGIN\n  SELECT 1;\n  SELECT 2;\n"
                                        "END;")
                             .arg(objType);
    const QStringList stmts =
        splitStatements(SchemaSql::editorText(objType, QStringLiteral("d"), QStringLiteral("o"),
                                              body, false, driver),
                        driver);
    QCOMPARE(stmts.size(), 2);
    QCOMPARE(stmts[0], QStringLiteral("DROP %1 IF EXISTS `d`.`o`").arg(objType));
    /* the compound body survives with its inner semicolons; the trailing one
     * SHOW CREATE / the user left on is dropped */
    QCOMPARE(stmts[1], body.chopped(1));
}

void TestSchemaSql::mysqlCreateHasNoDrop()
{
    const QString tpl = SchemaSql::createTemplate(QStringLiteral("PROCEDURE"), QStringLiteral("d"));
    const QStringList stmts = splitStatements(SchemaSql::editorText(
        QStringLiteral("PROCEDURE"), QStringLiteral("d"), QStringLiteral("new_proc"), tpl, true));
    QCOMPARE(stmts, QStringList{tpl});
}

void TestSchemaSql::postgresFunctionAlterIsOneStatement()
{
    /* pg_get_functiondef() shape: already CREATE OR REPLACE, body
     * dollar-quoted with its own semicolons, ends in the closing tag — the
     * case SchemaSql.cpp's "newline before the closing $$" comment is about */
    const QString def = QStringLiteral("CREATE OR REPLACE FUNCTION public.f(a integer)\n"
                                       " RETURNS integer\n LANGUAGE plpgsql\n"
                                       "AS $function$\nBEGIN\n  RETURN a;\nEND;\n$function$");
    const QStringList stmts = splitStatements(
        SchemaSql::editorText(QStringLiteral("FUNCTION"), QStringLiteral("public"),
                              QStringLiteral("f"), def, false, SqlDriverType::Postgres),
        SqlDriverType::Postgres);
    QCOMPARE(stmts, QStringList{def});
}

void TestSchemaSql::postgresCreateTemplatesStayWhole_data()
{
    QTest::addColumn<QString>("objType");
    QTest::newRow("FUNCTION") << "FUNCTION";
    QTest::newRow("PROCEDURE") << "PROCEDURE";
    /* two statements (trigger function + trigger), sent as one chunk */
    QTest::newRow("TRIGGER") << "TRIGGER";
}

void TestSchemaSql::postgresCreateTemplatesStayWhole()
{
    QFETCH(QString, objType);
    const QString tpl =
        SchemaSql::createTemplate(objType, QStringLiteral("public"), SqlDriverType::Postgres);
    QString body = tpl.trimmed();
    while(body.endsWith(QLatin1Char(';')))
        body.chop(1);
    const QStringList stmts =
        splitStatements(SchemaSql::editorText(objType, QStringLiteral("public"), QString(), tpl,
                                              true, SqlDriverType::Postgres),
                        SqlDriverType::Postgres);
    QCOMPARE(stmts, QStringList{body});
}

void TestSchemaSql::postgresTriggerAlterDropsOnItsTable_data()
{
    QTest::addColumn<QString>("def");
    QTest::addColumn<QString>("table");

    QTest::newRow("schema-qualified")
        << "CREATE TRIGGER trg BEFORE INSERT ON public.emp FOR EACH ROW EXECUTE FUNCTION f()"
        << "public.emp";
    QTest::newRow("quoted identifiers")
        << "CREATE TRIGGER trg BEFORE INSERT ON \"My Schema\".\"Emp Table\" FOR EACH ROW EXECUTE "
           "FUNCTION f()"
        << "\"My Schema\".\"Emp Table\"";
    QTest::newRow("bare table") << "CREATE TRIGGER trg AFTER UPDATE ON emp FOR EACH ROW EXECUTE "
                                   "FUNCTION f()"
                                << "emp";
}

void TestSchemaSql::postgresTriggerAlterDropsOnItsTable()
{
    QFETCH(QString, def);
    QFETCH(QString, table);

    const QStringList stmts = splitStatements(
        SchemaSql::editorText(QStringLiteral("TRIGGER"), QStringLiteral("public"),
                              QStringLiteral("trg"), def, false, SqlDriverType::Postgres),
        SqlDriverType::Postgres);
    QCOMPARE(stmts.size(), 2);
    QCOMPARE(stmts[0], QStringLiteral("DROP TRIGGER IF EXISTS \"trg\" ON %1").arg(table));
    QCOMPARE(stmts[1], def);
}

QTEST_GUILESS_MAIN(TestSchemaSql)
#include "tst_schemasql.moc"
