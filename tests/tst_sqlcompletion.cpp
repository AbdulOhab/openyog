/* OpenYog — unit tests for qt/SqlCompletion (what the query editor's
 * autocomplete offers, and in what order). Each case writes the caret as §. */
#include "SqlCompletion.h"

#include <QTest>

using SqlCompletion::Kind;

namespace {
const QChar kCaret(0x00A7); /* § */

SqlCompletion::Schema shopSchema()
{
    SqlCompletion::Schema s;
    s.database = QStringLiteral("shop");
    s.tables = {"employees", "emp_dept", "orders", "order"};
    s.views = {"v_employee_dept"};
    s.columns = {"emp_id", "emp_name", "hire_dt",       "dept_id",      "dept_name",
                 "o_id",   "o_total",  "customer_name", "aaa_unrelated"};
    s.databases = {"shop", "archive"};
    s.functions = {"calc_tax"};
    s.procedures = {"refresh_stats"};
    s.columnsOf = [](const QString &t) -> QStringList {
        if(t == QLatin1String("employees"))
            return {"emp_id", "emp_name", "hire_dt", "dept_id"};
        if(t == QLatin1String("emp_dept"))
            return {"dept_id", "dept_name"};
        if(t == QLatin1String("orders"))
            return {"o_id", "o_total", "emp_id", "customer_name"};
        return {};
    };
    s.tablesOf = [](const QString &db) -> QStringList {
        return db == QLatin1String("archive") ? QStringList{"old_orders"} : QStringList{};
    };
    return s;
}

QList<SqlCompletion::Item> itemsAt(QString sql, SqlDriverType d = SqlDriverType::Mysql,
                                   const SqlCompletion::Schema &schema = shopSchema())
{
    const int pos = int(sql.indexOf(kCaret));
    Q_ASSERT(pos >= 0);
    sql.remove(pos, 1);
    return SqlCompletion::complete(sql, pos, schema, d);
}

QStringList textsAt(const QString &sql, SqlDriverType d = SqlDriverType::Mysql,
                    const SqlCompletion::Schema &schema = shopSchema())
{
    QStringList out;
    for(const SqlCompletion::Item &i : itemsAt(sql, d, schema))
        out << i.text;
    return out;
}

QString firstAt(const QString &sql, SqlDriverType d = SqlDriverType::Mysql)
{
    return textsAt(sql, d).value(0);
}

bool inStringOrComment(QString sql, SqlDriverType d = SqlDriverType::Mysql)
{
    const int pos = int(sql.indexOf(kCaret));
    sql.remove(pos, 1);
    return SqlCompletion::inStringOrComment(sql, pos, d);
}
} // namespace

class TestSqlCompletion : public QObject
{
    Q_OBJECT

private slots:
    void tablesAfterFrom();
    void tablePrefixAndSubstring();
    void keywordsRightAfterATable();
    void ownColumnsLeadInSelect();
    void wordBoundaryAndFuzzyMatches();
    void qualifiedNames();
    void insertColumnList();
    void valuesOfferFunctions();
    void restOfAKeywordPhrase();
    void statementStartKeywords();
    void exactKeywordFirst();
    void operatorsAfterAnOperand();
    void dialectVocabulary();
    void nothingWhereANewNameGoes();
    void callAndUse();
    void alterTableColumns();
    void createTableTypes();
    void statementBoundariesAndComments();
    void quoting();
    void quotedNamesInCompletions();
    void stringsAndComments();
    void prefixAt();
};

void TestSqlCompletion::tablesAfterFrom()
{
    const QList<SqlCompletion::Item> items = itemsAt(QStringLiteral("SELECT * FROM §"));
    QStringList first;
    for(int i = 0; i < 5; ++i)
        first << items.value(i).text;
    /* tables (alphabetical, `order` quoted — it's reserved), then views */
    QCOMPARE(first, (QStringList{"emp_dept", "employees", "`order`", "orders", "v_employee_dept"}));
    QCOMPARE(items[0].kind, Kind::Table);
    QCOMPARE(items[4].kind, Kind::View);
    /* databases (for db.table) before keywords */
    QCOMPARE(items[5].kind, Kind::Database);
}

void TestSqlCompletion::tablePrefixAndSubstring()
{
    /* prefix matches first; v_employee_dept only contains "emp" */
    QCOMPARE(textsAt(QStringLiteral("SELECT * FROM emp§")),
             (QStringList{"emp_dept", "employees", "v_employee_dept"}));
}

void TestSqlCompletion::keywordsRightAfterATable()
{
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM employees wh§")), QStringLiteral("WHERE"));
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM employees e jo§")), QStringLiteral("JOIN"));
    QCOMPARE(firstAt(QStringLiteral("UPDATE orders s§")), QStringLiteral("SET"));
    QCOMPARE(itemsAt(QStringLiteral("DELETE FROM orders §")).value(0).kind, Kind::Keyword);
}

void TestSqlCompletion::ownColumnsLeadInSelect()
{
    const QStringList t = textsAt(QStringLiteral("SELECT § FROM employees e"));
    QCOMPARE(t.mid(0, 5), (QStringList{"emp_id", "emp_name", "hire_dt", "dept_id", "e"}));
    /* everything else still follows */
    QVERIFY(t.contains(QStringLiteral("aaa_unrelated")));
    QVERIFY(t.contains(QStringLiteral("COUNT(")));

    const QStringList w = textsAt(QStringLiteral("SELECT * FROM orders WHERE §"));
    QCOMPARE(w.mid(0, 4), (QStringList{"o_id", "o_total", "emp_id", "customer_name"}));
}

void TestSqlCompletion::wordBoundaryAndFuzzyMatches()
{
    /* "name" finds emp_name (the statement's own table) first, then the
     * other tables' *_name columns */
    const QStringList n = textsAt(QStringLiteral("SELECT name§ FROM employees"));
    QCOMPARE(n.value(0), QStringLiteral("emp_name"));
    QVERIFY(n.contains(QStringLiteral("customer_name")));
    QVERIFY(n.contains(QStringLiteral("dept_name")));
    /* in-order letters */
    QVERIFY(textsAt(QStringLiteral("SELECT ename§ FROM employees"))
                .contains(QStringLiteral("emp_name")));
    /* but never buried letters of a keyword */
    QVERIFY(!textsAt(QStringLiteral("SELECT * FROM emp§"))
                 .contains(QStringLiteral("CREATE TEMPORARY TABLE")));
}

void TestSqlCompletion::qualifiedNames()
{
    const QString sql =
        QStringLiteral("SELECT %1 FROM employees e JOIN orders o ON e.emp_id = o.emp_id");
    QCOMPARE(textsAt(sql.arg(QStringLiteral("e.§"))),
             (QStringList{"emp_id", "emp_name", "hire_dt", "dept_id"}));
    QCOMPARE(textsAt(sql.arg(QStringLiteral("o.cu§"))), (QStringList{"customer_name"}));
    /* a table the statement doesn't name yet */
    QCOMPARE(textsAt(QStringLiteral("SELECT emp_dept.§")), (QStringList{"dept_id", "dept_name"}));
    /* db. → that database's tables */
    QCOMPARE(textsAt(QStringLiteral("SELECT * FROM archive.§")), (QStringList{"old_orders"}));
    QVERIFY(textsAt(QStringLiteral("SELECT * FROM shop.§")).contains(QStringLiteral("employees")));
    /* nothing for a number or an unknown name */
    QVERIFY(textsAt(QStringLiteral("SELECT 3.§")).isEmpty());
    QVERIFY(textsAt(QStringLiteral("SELECT x.§ FROM employees")).isEmpty());
}

void TestSqlCompletion::insertColumnList()
{
    const QStringList cols{"o_id", "o_total", "emp_id", "customer_name"};
    QCOMPARE(textsAt(QStringLiteral("INSERT INTO orders (§")), cols);
    QCOMPARE(textsAt(QStringLiteral("INSERT INTO orders (o_id, §")), cols);
    QCOMPARE(textsAt(QStringLiteral("INSERT INTO shop.orders (§")), cols);
}

void TestSqlCompletion::valuesOfferFunctions()
{
    QCOMPARE(firstAt(QStringLiteral("INSERT INTO orders (o_id) VALUES (NO§")),
             QStringLiteral("NOW()"));
    QVERIFY(textsAt(QStringLiteral("INSERT INTO orders VALUES (1, nu§"))
                .contains(QStringLiteral("NULL")));
}

void TestSqlCompletion::restOfAKeywordPhrase()
{
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM orders GROUP §")), QStringLiteral("BY"));
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM orders ORDER b§")), QStringLiteral("BY"));
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM employees LEFT §")), QStringLiteral("JOIN"));
    QCOMPARE(firstAt(QStringLiteral("SHOW ta§")), QStringLiteral("TABLES"));
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM orders WHERE o_id IS §")),
             QStringLiteral("NULL"));
}

void TestSqlCompletion::statementStartKeywords()
{
    QCOMPARE(firstAt(QStringLiteral("sel§")), QStringLiteral("SELECT"));
    QCOMPARE(firstAt(QStringLiteral("ins§")), QStringLiteral("INSERT INTO"));
    QCOMPARE(firstAt(QStringLiteral("SELECT 1;\nup§")), QStringLiteral("UPDATE"));
}

void TestSqlCompletion::exactKeywordFirst()
{
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM orders WHERE o_id = 1 or§")),
             QStringLiteral("OR"));
}

void TestSqlCompletion::operatorsAfterAnOperand()
{
    QCOMPARE(firstAt(QStringLiteral("SELECT emp_id fr§")), QStringLiteral("FROM"));
    QCOMPARE(firstAt(QStringLiteral("SELECT COUNT(*) fr§")), QStringLiteral("FROM"));
    QCOMPARE(firstAt(QStringLiteral("SELECT * fr§")), QStringLiteral("FROM"));
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM orders WHERE o_total be§")),
             QStringLiteral("BETWEEN"));
    /* but after an operator, columns again */
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM orders WHERE o_total > §")),
             QStringLiteral("o_id"));
}

void TestSqlCompletion::dialectVocabulary()
{
    const QString ilike = QStringLiteral("SELECT * FROM orders WHERE customer_name ili§");
    QCOMPARE(firstAt(ilike, SqlDriverType::Postgres), QStringLiteral("ILIKE"));
    QVERIFY(!textsAt(ilike, SqlDriverType::Mysql).contains(QStringLiteral("ILIKE")));

    QCOMPARE(firstAt(QStringLiteral("SELECT date_tr§"), SqlDriverType::Postgres),
             QStringLiteral("DATE_TRUNC("));
    QCOMPARE(firstAt(QStringLiteral("SELECT date_f§"), SqlDriverType::Mysql),
             QStringLiteral("DATE_FORMAT("));
    QCOMPARE(firstAt(QStringLiteral("SELECT strf§"), SqlDriverType::Sqlite),
             QStringLiteral("STRFTIME("));
    QCOMPARE(firstAt(QStringLiteral("SELECT group_con§"), SqlDriverType::Sqlite),
             QStringLiteral("GROUP_CONCAT("));
    QVERIFY(!textsAt(QStringLiteral("SELECT group_con§"), SqlDriverType::Postgres)
                 .contains(QStringLiteral("GROUP_CONCAT(")));
    /* the database's own functions, ready for their arguments */
    QCOMPARE(firstAt(QStringLiteral("SELECT calc§ FROM orders")), QStringLiteral("calc_tax("));
}

void TestSqlCompletion::nothingWhereANewNameGoes()
{
    QVERIFY(textsAt(QStringLiteral("SELECT o_total AS §")).isEmpty());
    QVERIFY(textsAt(QStringLiteral("SELECT * FROM orders LIMIT §")).isEmpty());
    QVERIFY(textsAt(QStringLiteral("ALTER TABLE orders ADD COLUMN §")).isEmpty());
}

void TestSqlCompletion::callAndUse()
{
    QCOMPARE(textsAt(QStringLiteral("CALL §")), (QStringList{"refresh_stats("}));
    QCOMPARE(textsAt(QStringLiteral("USE §")), (QStringList{"archive", "shop"}));
}

void TestSqlCompletion::alterTableColumns()
{
    QCOMPARE(firstAt(QStringLiteral("ALTER TABLE orders DROP COLUMN §")), QStringLiteral("o_id"));
    QCOMPARE(firstAt(QStringLiteral("ALTER TABLE orders MODIFY cu§")),
             QStringLiteral("customer_name"));
    QCOMPARE(firstAt(QStringLiteral("ALTER TABLE orders ad§")), QStringLiteral("ADD COLUMN"));
}

void TestSqlCompletion::createTableTypes()
{
    QCOMPARE(firstAt(QStringLiteral("CREATE TABLE x (id INT, name varc§")),
             QStringLiteral("VARCHAR("));
    QCOMPARE(firstAt(QStringLiteral("CREATE TABLE IF NOT EXISTS x (id serial, d jsonb, t time§"),
                     SqlDriverType::Postgres),
             QStringLiteral("TIME"));
}

void TestSqlCompletion::statementBoundariesAndComments()
{
    /* a ; inside a string doesn't end the statement */
    QCOMPARE(firstAt(QStringLiteral("SELECT 'a;b' FROM orders WHERE §")), QStringLiteral("o_id"));
    /* a comment between the clause keyword and the caret is skipped */
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM orders -- the from part\nWHERE §")),
             QStringLiteral("o_id"));
    /* the previous statement's tables don't leak in */
    QCOMPARE(firstAt(QStringLiteral("SELECT * FROM orders;\nSELECT * FROM employees WHERE §")),
             QStringLiteral("emp_id"));
}

void TestSqlCompletion::quoting()
{
    using SqlCompletion::quoteIfNeeded;
    QCOMPARE(quoteIfNeeded(QStringLiteral("orders"), SqlDriverType::Mysql),
             QStringLiteral("orders"));
    QCOMPARE(quoteIfNeeded(QStringLiteral("order"), SqlDriverType::Mysql),
             QStringLiteral("`order`"));
    QCOMPARE(quoteIfNeeded(QStringLiteral("order"), SqlDriverType::Postgres),
             QStringLiteral("\"order\""));
    QCOMPARE(quoteIfNeeded(QStringLiteral("my table"), SqlDriverType::Sqlite),
             QStringLiteral("\"my table\""));
    QCOMPARE(quoteIfNeeded(QStringLiteral("a`b"), SqlDriverType::Mysql), QStringLiteral("`a``b`"));
    /* PostgreSQL folds unquoted names to lower case */
    QCOMPARE(quoteIfNeeded(QStringLiteral("Employees"), SqlDriverType::Postgres),
             QStringLiteral("\"Employees\""));
    QCOMPARE(quoteIfNeeded(QStringLiteral("Employees"), SqlDriverType::Mysql),
             QStringLiteral("Employees"));
}

void TestSqlCompletion::quotedNamesInCompletions()
{
    SqlCompletion::Schema pg;
    pg.tables = {"Employees", "orders"};
    QCOMPARE(textsAt(QStringLiteral("SELECT * FROM emp§"), SqlDriverType::Postgres, pg),
             (QStringList{"\"Employees\""}));
    /* the user already opened the quote: the bare name */
    QCOMPARE(textsAt(QStringLiteral("SELECT * FROM \"Emp§"), SqlDriverType::Postgres, pg),
             (QStringList{"Employees"}));
    QVERIFY(textsAt(QStringLiteral("SELECT * FROM `or§")).contains(QStringLiteral("order")));
}

void TestSqlCompletion::stringsAndComments()
{
    QVERIFY(inStringOrComment(QStringLiteral("SELECT 'abc§")));
    QVERIFY(!inStringOrComment(QStringLiteral("SELECT 'abc' §")));
    QVERIFY(inStringOrComment(QStringLiteral("SELECT 1 -- note §")));
    QVERIFY(!inStringOrComment(QStringLiteral("SELECT 1 -- note\n§")));
    QVERIFY(inStringOrComment(QStringLiteral("SELECT /* x §")));
    QVERIFY(inStringOrComment(QStringLiteral("SELECT 'it\\'s §")));
    /* "…" is a string on MySQL, an identifier elsewhere */
    QVERIFY(inStringOrComment(QStringLiteral("SELECT \"ab§")));
    QVERIFY(!inStringOrComment(QStringLiteral("SELECT \"ab§"), SqlDriverType::Postgres));
    QVERIFY(inStringOrComment(QStringLiteral("SELECT $$ body §"), SqlDriverType::Postgres));
    /* a quote in an earlier statement is closed and gone */
    QVERIFY(!inStringOrComment(QStringLiteral("SELECT 'a';\nSELECT §")));
}

void TestSqlCompletion::prefixAt()
{
    QCOMPARE(SqlCompletion::prefixAt(QStringLiteral("SELECT e.na"), 11), QStringLiteral("na"));
    QCOMPARE(SqlCompletion::prefixAt(QStringLiteral("SELECT "), 7), QString());
    QCOMPARE(SqlCompletion::prefixAt(QStringLiteral("SELECT emp_id"), 10), QStringLiteral("emp"));
}

QTEST_GUILESS_MAIN(TestSqlCompletion)
#include "tst_sqlcompletion.moc"
