/* OpenYog — the Query Builder tab (File ▸ New Query Builder): a SELECT put
 * together by pointing instead of typing.
 *
 * Tables come from the list on the left, by double-click or by dragging them
 * onto the canvas; a table added next to one it has a foreign key with is
 * joined to it straight away, and dragging a column onto another table's
 * column joins them by hand (right-click a join for LEFT / RIGHT / remove).
 * Ticking a column's box puts it in the grid underneath, where it gets an
 * alias, an aggregate, a sort and a condition. The SQL underneath follows
 * every change, and goes to a query editor or straight to the server from
 * the toolbar. What has been built is saved to and opened from a .oyqb file
 * (JSON). QueryBuild writes the SQL; this is the pointing. */
#pragma once

#include "DiagramItems.h"
#include "QueryBuild.h"
#include "SchemaDiagram.h"

#include <QHash>
#include <QWidget>

#include <functional>

class QCheckBox;
class QGraphicsScene;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QTableWidget;
class SqlEditor;

class QueryBuilder : public QWidget
{
    Q_OBJECT
public:
    /* what it reads from the connection */
    struct Backend
    {
        std::function<QStringList()> tables;                        /* tables and views */
        std::function<SchemaDiagram::Table(const QString &)> table; /* one's columns */
        std::function<QList<SchemaDiagram::Link>()> foreignKeys;
        std::function<QString(const QString &)> quote;
    };

    QueryBuilder(const QString &database, Backend backend, QWidget *parent = nullptr);

    QString database() const
    {
        return m_database;
    }
    QString sql() const;

    /* add a table (an alias when it is already there) and tick `columns`;
     * what a double-click on the list and the --querybuilder selftest do */
    void addTable(const QString &table, const QStringList &columns = {});

    /* the query and where its tables are on the canvas, as a .oyqb file */
    bool saveFile(const QString &file, QString *error = nullptr);
    /* replaces what is built with a saved query, read against this
     * database's tables as they are now: a table, join or column that
     * isn't there any more is left out and named in *leftOut */
    bool openFile(const QString &file, QString *error = nullptr, QStringList *leftOut = nullptr);

signals:
    void copyToEditorRequested(const QString &sql);
    void runRequested(const QString &sql);

protected:
    void changeEvent(QEvent *event) override;

private:
    struct Instance
    {
        SchemaDiagram::Table def;
        QString alias; /* empty: the table's own name */
        QPointF pos;
    };

    void addTableAt(const QString &table, const QPointF &pos, bool placed);
    void savePositions();
    QPointF nextFreeSpot(const Instance &in) const;
    void clearAll();
    void autoJoin(int added);
    void addJoin(int left, const QString &leftColumn, int right, const QString &rightColumn);
    void removeSource(int index);
    void toggleColumn(int source, const QString &column, bool on);
    void rebuildCanvas();
    void rebuildGrid();
    void updateSql();
    void showCanvasMenu(const QPoint &viewPos);
    void showGridMenu(const QPoint &pos);
    void filterTables(const QString &text);
    void reloadTables();
    void promptSave();
    void promptOpen();
    QueryBuild::Query query() const;
    QString sourceTitle(int index) const;

    QString m_database;
    QString m_file; /* last saved to or opened from */
    Backend m_backend;
    QList<SchemaDiagram::Link> m_foreignKeys;
    QList<Instance> m_sources; /* index = QueryBuild source index */
    QList<QueryBuild::Join> m_joins;
    QList<QueryBuild::Column> m_columns; /* the grid's rows */
    QHash<Diagram::Table *, int> m_itemSource;
    QList<Diagram::Table *> m_items; /* per source */
    Diagram::Colors m_colors;
    bool m_fillingGrid = false;

    QGraphicsScene *m_scene = nullptr;
    Diagram::View *m_view = nullptr;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_tableList = nullptr;
    QTableWidget *m_grid = nullptr;
    SqlEditor *m_sql = nullptr;
    QCheckBox *m_distinct = nullptr;
    QSpinBox *m_limit = nullptr;
};
