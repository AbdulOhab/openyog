/* OpenYog — what the query editor's autocomplete offers, and in what order.
 *
 * Kept free of widgets so it can be unit tested: given the script, the caret
 * and what the schema holds, complete() returns ranked suggestions, and
 * SqlEditor only turns them into Scintilla's popup.
 *
 * The clause around the caret decides what's relevant (the statement is
 * found with SqlSplit's rules, so quotes, comments and DELIMITER behave the
 * same as on Execute):
 *   FROM / JOIN / INTO / UPDATE …      tables and views
 *   … right after such a table         clause keywords first (WHERE, ON, SET …)
 *   SELECT / WHERE / ON / SET / BY …   the statement's own tables' columns
 *                                      first, then aliases, other columns,
 *                                      functions, keywords
 *   … right after an operand           operators and clause keywords first
 *   alias. / table. / db.              that table's columns / that db's tables
 *   JOIN …                             tables a foreign key ties to the
 *                                      statement's tables first
 *   … JOIN t x ON                      the join condition(s) those foreign
 *                                      keys spell out: x.id = e.t_id
 *   INSERT INTO t (                    t's columns
 *   VALUES (                           functions and NULL / DEFAULT
 *   USE / CALL                         databases / stored procedures
 *   CREATE TABLE t (                   column types and constraints
 *   statement start                    statement keywords first
 *
 * Matching: a prefix match beats a match at a word boundary inside the name
 * ("name" → emp_name, "join" → LEFT JOIN), which beats a substring and then a
 * fuzzy (in-order letters) match — the last two for identifiers only, so a
 * keyword never turns up for letters buried in its middle. Keywords and
 * functions follow the connection's dialect. */
#pragma once

#include "ConnectionParams.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace SqlCompletion {

enum class Kind { Keyword, Function, Table, View, Column, Alias, Database, Procedure, Join };

struct Item
{
    QString text; /* exactly what gets inserted: quoted when the name needs it */
    Kind kind;
};

/* one foreign key: table(columns) REFERENCES refTable(refColumns) */
struct ForeignKey
{
    QString table;
    QStringList columns;
    QString refTable;
    QStringList refColumns;
};

struct Schema
{
    QString database;   /* the current database (or schema) */
    QStringList tables; /* its base tables */
    QStringList views;
    QStringList columns; /* every column name in it: the pool for a bare column */
    QStringList databases;
    QStringList functions; /* its own stored functions */
    QStringList procedures;
    QStringList extra;             /* untyped words, offered where tables/columns are empty */
    QList<ForeignKey> foreignKeys; /* the database's, for JOIN suggestions */
    /* one table's columns in ordinal order; may be unset */
    std::function<QStringList(const QString &table)> columnsOf;
    /* another database's tables, for "db." ; may be unset */
    std::function<QStringList(const QString &database)> tablesOf;
};

/* the partial identifier ending at pos — "na" in "SELECT e.na|" */
QString prefixAt(const QString &sql, int pos);

/* true when pos is inside a string literal or a comment, where nothing should
 * pop up on its own (a quoted identifier doesn't count) */
bool inStringOrComment(const QString &sql, int pos, SqlDriverType driver);

/* ranked suggestions for the caret at pos (best first, no duplicates) */
QList<Item> complete(const QString &sql, int pos, const Schema &schema, SqlDriverType driver);

/* `ident` as it has to be written: quoted when it isn't a plain identifier, is
 * a reserved word, or (PostgreSQL, which folds unquoted names to lower case)
 * has upper-case letters */
QString quoteIfNeeded(const QString &ident, SqlDriverType driver);

} // namespace SqlCompletion
