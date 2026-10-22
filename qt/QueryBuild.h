/* OpenYog — the SELECT the Query Builder writes, from what has been put
 * together on its canvas and in its grid: the tables (a table used twice
 * gets an alias), the joins between them, and the columns with their
 * aliases, aggregates, sort and conditions.
 *
 * Conditions are SQL as typed ("> 100", "LIKE 'A%'", "IS NULL"); one that
 * starts with a value instead of an operator compares with "=". Nothing in a
 * condition is quoted or escaped: it goes into the query as written. */
#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

namespace QueryBuild {

enum class JoinType {
    Inner,
    Left, /* every row of `left` */
    Right /* every row of `right` */
};

enum class Sort { None, Asc, Desc };

struct Source
{
    QString table;
    QString alias; /* empty: the table's own name */

    QString ref() const
    {
        return alias.isEmpty() ? table : alias;
    }
};

/* between two sources (indexes into Query::sources); several column pairs
 * are ANDed */
struct Join
{
    int left = 0;
    int right = 0;
    QList<QPair<QString, QString>> on; /* left column, right column */
    JoinType type = JoinType::Inner;
};

struct Column
{
    int source = 0;
    QString column;
    QString alias;
    QString aggregate; /* "", COUNT, SUM, AVG, MIN, MAX */
    Sort sort = Sort::None;
    QString criteria;
    bool output = true; /* false: only there for its condition or sort */
};

struct Query
{
    QString database; /* qualifies each table in FROM when set */
    QList<Source> sources;
    QList<Join> joins;
    QList<Column> columns;
    bool distinct = false;
    int limit = 0; /* 0: none */
};

/* quotes an identifier the way the connection's dialect does */
using Quote = std::function<QString(const QString &)>;

/* the query, laid out one clause per line; empty when there are no sources.
 * The first source is FROM; every other one joins on its conditions with the
 * sources before it (CROSS JOIN when it has none). Aggregates group by the
 * other output columns; a condition on an aggregate goes in HAVING. */
QString toSql(const Query &query, const Quote &quote);

/* `expression` compared as `criteria` says: "> 5" → "expr > 5",
 * "5" → "expr = 5"; empty when criteria is */
QString condition(const QString &expression, const QString &criteria);

} // namespace QueryBuild
