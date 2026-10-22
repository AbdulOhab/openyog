/* OpenYog — the SELECT the Query Builder writes, from what has been put
 * together on its canvas and in its grid: the tables (a table used twice
 * gets an alias), the joins between them, and the columns with their
 * aliases, aggregates, sort and conditions.
 *
 * Conditions are SQL as typed ("> 100", "LIKE 'A%'", "IS NULL"); one that
 * starts with a value instead of an operator compares with "=". Nothing in a
 * condition is quoted or escaped: it goes into the query as written.
 *
 * The grid's Criteria and Or are two sets of conditions, the way a QBE grid
 * has them: a row is kept when it meets every condition of one set or every
 * condition of the other.
 *
 * A query is saved as JSON (toJson / fromJson), the Query Builder's .oyqb
 * files. */
#pragma once

#include <QJsonObject>
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
    QString orCriteria; /* the second set of conditions */
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
 * other output columns; a condition on an aggregate goes in HAVING. With
 * both sets of conditions in use, each set's plain conditions are ORed in
 * WHERE and its aggregate ones in HAVING; when a set has an aggregate
 * condition and either has a plain one the sets can't be split that way and
 * go in HAVING whole, where a plain condition must be on a grouped column. */
QString toSql(const Query &query, const Quote &quote);

/* a query as JSON and back; fromJson leaves out joins and columns whose
 * sources aren't there, and says false (with *error) for what isn't a
 * saved query at all */
QJsonObject toJson(const Query &query);
bool fromJson(const QJsonObject &json, Query *query, QString *error = nullptr);

/* `expression` compared as `criteria` says: "> 5" → "expr > 5",
 * "5" → "expr = 5"; empty when criteria is */
QString condition(const QString &expression, const QString &criteria);

} // namespace QueryBuild
