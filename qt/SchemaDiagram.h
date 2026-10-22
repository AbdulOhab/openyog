/* OpenYog — what the Schema Designer draws, apart from the drawing: a
 * database's tables, their columns and the foreign keys between them, read
 * through IDbConnection, and where to put each table on the canvas.
 *
 * The layout reads like a family tree on its side: a table sits to the right
 * of every table it references, so the tables nothing references end up on
 * the right and the lookup tables they point at on the left. Tables joined by
 * foreign keys are laid out together; tables with none are packed after
 * them. The result depends only on the model and the table sizes, never on
 * iteration order or chance, so the same schema always looks the same. */
#pragma once

#include <QHash>
#include <QList>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QStringList>

class IDbConnection;
struct DbResultSet;

namespace SchemaDiagram {

struct Column
{
    QString name;
    QString type;
    bool primaryKey = false;
    bool foreignKey = false;
    bool notNull = false;
};

struct Table
{
    QString name;
    QList<Column> columns;
};

/* one foreign key; a composite key lists its columns in order */
struct Link
{
    QString name;
    QString table;
    QStringList columns;
    QString refTable;
    QStringList refColumns;
};

struct Model
{
    QList<Table> tables; /* in listTables() order */
    QList<Link> links;   /* only those whose two tables are both in `tables` */
};

/* the base tables of `db` (no views), their columns and foreign keys */
Model load(IDbConnection *conn, const QString &db);

/* one table or view and its columns, foreign key columns marked; no columns
 * when there is no such table */
Table loadTable(IDbConnection *conn, const QString &db, const QString &table);

/* the foreign keys between `db`'s base tables, without their columns */
QList<Link> loadForeignKeys(IDbConnection *conn, const QString &db);

/* load()'s assembly, from listColumns() rows per table (SHOW COLUMNS shape)
 * and listAllForeignKeys() rows: table, constraint, column, referenced
 * table, referenced column */
Model fromRows(const QStringList &tables, const QHash<QString, DbResultSet> &columns,
               const DbResultSet &foreignKeys);

/* top-left corner of each table, given each table's size on the canvas */
QHash<QString, QPointF> layout(const Model &model, const QHash<QString, QSizeF> &sizes);

} // namespace SchemaDiagram
