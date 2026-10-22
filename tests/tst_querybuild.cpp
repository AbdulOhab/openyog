/* OpenYog — unit tests for qt/QueryBuild: the SELECT the Query Builder
 * writes. */
#include "QueryBuild.h"

#include <QJsonArray>
#include <QTest>

using namespace QueryBuild;

namespace {
QString bt(const QString &s)
{
    return QLatin1Char('`') + s + QLatin1Char('`');
}

Column col(int source, const QString &name)
{
    Column c;
    c.source = source;
    c.column = name;
    return c;
}
} // namespace

class TestQueryBuild : public QObject
{
    Q_OBJECT
private slots:
    void conditions()
    {
        QCOMPARE(condition("x", "> 5"), QStringLiteral("x > 5"));
        QCOMPARE(condition("x", ">=5"), QStringLiteral("x >=5"));
        QCOMPARE(condition("x", "<> 'a'"), QStringLiteral("x <> 'a'"));
        QCOMPARE(condition("x", "5"), QStringLiteral("x = 5"));
        QCOMPARE(condition("x", "'Dhaka'"), QStringLiteral("x = 'Dhaka'"));
        QCOMPARE(condition("x", "is null"), QStringLiteral("x is null"));
        QCOMPARE(condition("x", "LIKE 'A%'"), QStringLiteral("x LIKE 'A%'"));
        QCOMPARE(condition("x", "IN(1, 2)"), QStringLiteral("x IN(1, 2)"));
        QCOMPARE(condition("x", "between 1 and 3"), QStringLiteral("x between 1 and 3"));
        /* a word that merely starts like an operator is a value */
        QCOMPARE(condition("x", "isabel"), QStringLiteral("x = isabel"));
        QCOMPARE(condition("x", "   "), QString());
    }

    void oneTable()
    {
        Query q;
        QCOMPARE(toSql(q, bt), QString());
        q.sources = {{"employees", {}}};
        QCOMPARE(toSql(q, bt), QStringLiteral("SELECT *\nFROM `employees`"));

        Column name = col(0, "name");
        Column salary = col(0, "salary");
        salary.alias = "pay";
        salary.sort = Sort::Desc;
        salary.criteria = "> 1000";
        Column city = col(0, "city");
        city.output = false;
        city.criteria = "'Dhaka'";
        city.sort = Sort::Asc;
        q.columns = {name, salary, city};
        q.distinct = true;
        q.limit = 50;
        QCOMPARE(toSql(q, bt), QStringLiteral("SELECT DISTINCT\n"
                                              "    `employees`.`name`,\n"
                                              "    `employees`.`salary` AS `pay`\n"
                                              "FROM `employees`\n"
                                              "WHERE `employees`.`salary` > 1000\n"
                                              "    AND `employees`.`city` = 'Dhaka'\n"
                                              "ORDER BY `pay` DESC, `employees`.`city`\n"
                                              "LIMIT 50"));
    }

    void innerAndOuterJoins()
    {
        Query q;
        q.sources = {{"employees", {}}, {"emp_dept", {}}};
        q.joins = {{0, 1, {{"dept_id", "id"}}, JoinType::Inner}};
        q.columns = {col(0, "name"), col(1, "name")};
        q.columns[1].alias = "dept";
        QCOMPARE(
            toSql(q, bt),
            QStringLiteral("SELECT\n"
                           "    `employees`.`name`,\n"
                           "    `emp_dept`.`name` AS `dept`\n"
                           "FROM `employees`\n"
                           "    INNER JOIN `emp_dept` ON `emp_dept`.`id` = `employees`.`dept_id`"));

        /* every employee: LEFT, written from the employees side */
        q.joins[0].type = JoinType::Left;
        QVERIFY(toSql(q, bt).contains("\n    LEFT JOIN `emp_dept` ON"));
        /* every department: the join keeps its right side */
        q.joins[0].type = JoinType::Right;
        QVERIFY(toSql(q, bt).contains("\n    RIGHT JOIN `emp_dept` ON"));
        /* the same join defined the other way round flips with it */
        q.joins = {{1, 0, {{"id", "dept_id"}}, JoinType::Left}};
        QVERIFY(toSql(q, bt).contains(
            "\n    RIGHT JOIN `emp_dept` ON `emp_dept`.`id` = `employees`.`dept_id`"));
    }

    void selfJoinAndCompositeKeys()
    {
        Query q;
        q.sources = {{"employees", {}}, {"employees", "boss"}};
        q.joins = {{0, 1, {{"boss_id", "id"}, {"dept_id", "dept_id"}}, JoinType::Left}};
        q.columns = {col(0, "name"), col(1, "name")};
        q.columns[1].alias = "boss_name";
        QCOMPARE(
            toSql(q, bt),
            QStringLiteral("SELECT\n"
                           "    `employees`.`name`,\n"
                           "    `boss`.`name` AS `boss_name`\n"
                           "FROM `employees`\n"
                           "    LEFT JOIN `employees` AS `boss` ON `boss`.`id` = "
                           "`employees`.`boss_id` AND `boss`.`dept_id` = `employees`.`dept_id`"));
    }

    void joinOrderAndCrossJoins()
    {
        /* c is added before b but only joins through it: b comes first;
         * d joins nothing: CROSS JOIN */
        Query q;
        q.sources = {{"a", {}}, {"c", {}}, {"b", {}}, {"d", {}}};
        q.joins = {{2, 0, {{"a_id", "id"}}, JoinType::Inner},
                   {1, 2, {{"b_id", "id"}}, JoinType::Inner}};
        QCOMPARE(toSql(q, bt), QStringLiteral("SELECT *\n"
                                              "FROM `a`\n"
                                              "    INNER JOIN `b` ON `b`.`a_id` = `a`.`id`\n"
                                              "    INNER JOIN `c` ON `c`.`b_id` = `b`.`id`\n"
                                              "    CROSS JOIN `d`"));

        /* a triangle: the last table joins both others in one ON */
        q.sources = {{"a", {}}, {"b", {}}, {"c", {}}};
        q.joins = {{0, 1, {{"id", "a_id"}}, JoinType::Inner},
                   {1, 2, {{"id", "b_id"}}, JoinType::Inner},
                   {0, 2, {{"id", "a_id"}}, JoinType::Inner}};
        QVERIFY(toSql(q, bt).endsWith(
            "INNER JOIN `c` ON `c`.`b_id` = `b`.`id` AND `c`.`a_id` = `a`.`id`"));
    }

    void aggregates()
    {
        Query q;
        q.sources = {{"employees", {}}, {"emp_dept", {}}};
        q.joins = {{0, 1, {{"dept_id", "id"}}, JoinType::Inner}};
        Column dept = col(1, "name");
        Column count = col(0, "id");
        count.aggregate = "count";
        count.alias = "staff";
        count.criteria = "> 1";
        count.sort = Sort::Desc;
        Column pay = col(0, "salary");
        pay.aggregate = "AVG";
        Column hired = col(0, "hired");
        hired.output = false;
        hired.criteria = ">= '2023-01-01'";
        q.columns = {dept, count, pay, hired};
        QCOMPARE(
            toSql(q, bt),
            QStringLiteral("SELECT\n"
                           "    `emp_dept`.`name`,\n"
                           "    COUNT(`employees`.`id`) AS `staff`,\n"
                           "    AVG(`employees`.`salary`)\n"
                           "FROM `employees`\n"
                           "    INNER JOIN `emp_dept` ON `emp_dept`.`id` = `employees`.`dept_id`\n"
                           "WHERE `employees`.`hired` >= '2023-01-01'\n"
                           "GROUP BY `emp_dept`.`name`\n"
                           "HAVING COUNT(`employees`.`id`) > 1\n"
                           "ORDER BY `staff` DESC"));
    }

    void qualifiedByDatabase()
    {
        Query q;
        q.database = "shop";
        q.sources = {{"orders", {}}, {"orders", "o2"}};
        q.joins = {{0, 1, {{"parent_id", "id"}}, JoinType::Inner}};
        q.columns = {col(0, "id"), col(1, "id")};
        QCOMPARE(toSql(q, bt), QStringLiteral("SELECT\n"
                                              "    `orders`.`id`,\n"
                                              "    `o2`.`id`\n"
                                              "FROM `shop`.`orders`\n"
                                              "    INNER JOIN `shop`.`orders` AS `o2` ON "
                                              "`o2`.`id` = `orders`.`parent_id`"));
    }

    void orConditions()
    {
        Query q;
        q.sources = {{"employees", {}}};
        Column name = col(0, "name"), city = col(0, "city"), salary = col(0, "salary");
        name.criteria = "LIKE 'A%'";
        city.criteria = "'Dhaka'";
        salary.orCriteria = "IS NULL";
        q.columns = {name, city, salary};
        /* (name AND city) OR salary */
        QCOMPARE(toSql(q, bt),
                 QStringLiteral("SELECT\n"
                                "    `employees`.`name`,\n"
                                "    `employees`.`city`,\n"
                                "    `employees`.`salary`\n"
                                "FROM `employees`\n"
                                "WHERE (`employees`.`name` LIKE 'A%' AND `employees`.`city` = "
                                "'Dhaka')\n"
                                "    OR `employees`.`salary` IS NULL"));

        /* only the Or set used: as if it were the Criteria */
        q.columns[0].criteria.clear();
        q.columns[1].criteria.clear();
        QVERIFY(toSql(q, bt).endsWith("\nWHERE `employees`.`salary` IS NULL"));
        /* a column in both sets: a value either way */
        q.columns[1].criteria = "'Dhaka'";
        q.columns[1].orCriteria = "'Sylhet'";
        q.columns[2].orCriteria.clear();
        QVERIFY(toSql(q, bt).endsWith("\nWHERE `employees`.`city` = 'Dhaka'\n"
                                      "    OR `employees`.`city` = 'Sylhet'"));
    }

    void orConditionsOnAggregates()
    {
        Query q;
        q.sources = {{"employees", {}}};
        Column city = col(0, "city"), count = col(0, "id"), pay = col(0, "salary");
        count.aggregate = "COUNT";
        pay.aggregate = "SUM";
        count.criteria = "> 5";
        pay.orCriteria = "> 10000";
        q.columns = {city, count, pay};
        /* aggregate conditions only: both sets in HAVING, nothing in WHERE */
        QString sql = toSql(q, bt);
        QVERIFY(!sql.contains("WHERE"));
        QVERIFY2(sql.endsWith("GROUP BY `employees`.`city`\n"
                              "HAVING COUNT(`employees`.`id`) > 5\n"
                              "    OR SUM(`employees`.`salary`) > 10000"),
                 qPrintable(sql));

        /* a plain condition in one set, an aggregate one in the other: no
         * split into WHERE and HAVING keeps the OR, so all of it in HAVING */
        q.columns[0].criteria = "'Dhaka'";
        sql = toSql(q, bt);
        QVERIFY(!sql.contains("WHERE"));
        QVERIFY2(sql.endsWith("HAVING (`employees`.`city` = 'Dhaka' AND COUNT(`employees`.`id`) "
                              "> 5)\n"
                              "    OR SUM(`employees`.`salary`) > 10000"),
                 qPrintable(sql));

        /* without an Or set: WHERE and HAVING as before */
        q.columns[2].orCriteria.clear();
        sql = toSql(q, bt);
        QVERIFY2(sql.contains("\nWHERE `employees`.`city` = 'Dhaka'\nGROUP BY") &&
                     sql.endsWith("\nHAVING COUNT(`employees`.`id`) > 5"),
                 qPrintable(sql));
    }

    void savedAndOpened()
    {
        Query q;
        q.database = "shop";
        q.sources = {{"employees", {}}, {"emp_dept", "d"}};
        q.joins = {{0, 1, {{"dept_id", "id"}}, JoinType::Left}};
        Column name = col(0, "name"), dept = col(1, "name"), pay = col(0, "salary");
        name.alias = "who";
        name.sort = Sort::Desc;
        name.criteria = "LIKE 'A%'";
        dept.orCriteria = "'Sales'";
        pay.aggregate = "MAX";
        pay.output = false;
        q.columns = {name, dept, pay};
        q.distinct = true;
        q.limit = 25;

        Query back;
        QString error;
        QVERIFY2(fromJson(toJson(q), &back, &error), qPrintable(error));
        QCOMPARE(toSql(back, bt), toSql(q, bt));
        QCOMPARE(back.database, QStringLiteral("shop"));
        QCOMPARE(back.columns.size(), 3);
        QCOMPARE(back.columns[2].output, false);
        QCOMPARE(back.columns[1].orCriteria, QStringLiteral("'Sales'"));

        /* what refers to a source that isn't there is left out */
        QJsonObject json = toJson(q);
        QJsonArray columns = json.value("columns").toArray();
        QJsonObject stray = columns.at(0).toObject();
        stray.insert("source", 7);
        columns.append(stray);
        json.insert("columns", columns);
        QJsonArray joins = json.value("joins").toArray();
        joins.append(
            QJsonObject{{"left", 0}, {"right", 5}, {"on", QJsonArray{QJsonArray{"a", "b"}}}});
        json.insert("joins", joins);
        QVERIFY(fromJson(json, &back));
        QCOMPARE(back.columns.size(), 3);
        QCOMPARE(back.joins.size(), 1);

        QVERIFY(!fromJson(QJsonObject{{"positions", QJsonObject()}}, &back, &error));
        QVERIFY(error.contains("not a saved"));
    }

    void dialectQuoting()
    {
        Query q;
        q.sources = {{"order items", {}}};
        q.columns = {col(0, "unit price")};
        const auto dq = [](const QString &s) { return QLatin1Char('"') + s + QLatin1Char('"'); };
        QCOMPARE(
            toSql(q, dq),
            QStringLiteral("SELECT\n    \"order items\".\"unit price\"\nFROM \"order items\""));
    }
};

QTEST_GUILESS_MAIN(TestQueryBuild)
#include "tst_querybuild.moc"
