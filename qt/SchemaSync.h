/* OpenYog — what Schema Sync (Powertools ▸ Schema Synchronization Tool)
 * compares and the script it writes: the tables, views, routines and
 * triggers of a source database and a target one of the same kind (MySQL,
 * PostgreSQL or SQLite), and the DDL that makes the target's match the
 * source's.
 *
 * Each table is read the way its server writes it: MySQL's SHOW CREATE
 * TABLE, the PostgreSQL CREATE TABLE OpenYog reconstructs from the catalog
 * (the one its dumps use), SQLite's stored CREATE statements. Columns, keys
 * (primary key, unique and check constraints, indexes) and foreign keys are
 * compared by name and by that definition text, so a change is anything the
 * server would write differently.
 *
 * Views, routines and triggers are compared whole, by the CREATE statement
 * the server gives back, with what differs without meaning anything left out
 * (MySQL's DEFINER, the database's or schema's own name qualifying what is
 * in it); a different one is replaced (CREATE OR REPLACE where the dialect
 * has it and keeps what depends on it, DROP and CREATE elsewhere).
 *
 * Nothing here talks to the target: the script is for the user to read and
 * run. Changes that lose data (dropping a table or a column), dropping a
 * view, routine or trigger, and table option changes are left out unless
 * picked; what a dialect can't do in place
 * (SQLite altering a column) comes with a note instead of SQL. */
#pragma once

#include "ConnectionParams.h"

#include <QList>
#include <QString>
#include <QStringList>

class IDbConnection;

namespace SchemaSync {

/* a column, key or foreign key: its name and its definition as the dialect
 * writes it */
struct Part
{
    QString name;
    QString definition;
};

struct Table
{
    QString name;
    QString create; /* every statement that creates it, ";\n"-separated */
    QList<Part> columns;
    QList<Part> keys; /* "PRIMARY", unique and check constraints, indexes */
    QList<Part> foreignKeys;
    QString options; /* MySQL: ENGINE=… DEFAULT CHARSET=… */
};

/* a view, routine or trigger */
struct Object
{
    QString kind; /* VIEW, PROCEDURE, FUNCTION, TRIGGER */
    QString name;
    QString create; /* as the server writes it */
};

struct Schema
{
    SqlDriverType driver = SqlDriverType::Mysql;
    QString database; /* PostgreSQL: the schema */
    QList<Table> tables;
    /* routines, then views (each after the views it reads), then triggers:
     * an order they can be created in */
    QList<Object> objects;
};

/* one table from SHOW CREATE TABLE */
Table parseMySql(const QString &name, const QString &showCreate);
/* one table from PostgresConnection::showCreate("TABLE") and its
 * showCreate("FOREIGN KEYS") */
Table parsePostgres(const QString &name, const QString &create, const QString &foreignKeys);
/* one table from its CREATE TABLE and its indexes' CREATE INDEX statements */
Table parseSqlite(const QString &name, const QString &createTable,
                  const QStringList &createIndexes);

/* every base table, view, routine and trigger of `db` */
Schema load(IDbConnection *conn, const QString &db);

/* `views` (name, CREATE) each after the views it names */
QList<Object> orderViews(const QList<Object> &views);

enum class Action {
    CreateTable,
    DropTable,
    AddColumn,
    AlterColumn,
    DropColumn,
    AddKey,
    AlterKey,
    DropKey,
    AddForeignKey,
    AlterForeignKey,
    DropForeignKey,
    AlterOptions,
    CreateObject, /* a view, routine or trigger: Change::kind says which */
    AlterObject,
    DropObject
};

struct Change
{
    Action action = Action::CreateTable;
    QString table;          /* the table, or the view / routine / trigger */
    QString kind;           /* VIEW, PROCEDURE, FUNCTION, TRIGGER; empty for a table's */
    QString name;           /* the column / key / foreign key; empty for tables */
    QString source, target; /* the definitions; empty on the side without it */
    QStringList prepare;    /* runs with the drops (an altered key's DROP) */
    QStringList sql;        /* the rest; with `prepare`, empty when it can't be done */
    QString note;           /* why it can't be done, when it can't */
    bool compound = false;  /* `sql` has bodies of its own statements: DELIMITER $$ around */
    bool pickedByDefault = true;
};

/* what turns `target` into `source` (same driver): tables in the source's
 * order with their changes, then the target's extra tables */
QList<Change> compare(const Schema &source, const Schema &target);

/* the changes as one script, in an order every statement can run in: views,
 * routines and triggers dropped first, then foreign keys; missing tables
 * created before foreign keys are added; then routines, views and triggers
 * created, in that order */
QString script(const QList<Change> &changes, const Schema &source, const Schema &target);

/* "add column `bonus`", for the list of changes */
QString describe(const Change &change);

} // namespace SchemaSync
