/* OpenYog — the left-side Object Browser: filter box + tree
 * (connection → databases → Tables/Views/Stored Procs/Functions/Triggers/Events),
 * mirroring upstream ObjectBrowser.cpp's structure. Lazy-loads children on
 * expand; folders other than Tables arrive in later phases. */
#pragma once

#include <QColor>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QWidget>

#include <functional>

class IDbConnection;

/* Edit > Change Object Browser Color: persisted in OpenYog.ini
 * [UserInterface] browsercolor=#RRGGBB, same file Theme uses. An invalid
 * (default-constructed) QColor means "use the theme's own color" — no
 * override applied. */
namespace ObjectBrowserColor {
    QColor load();
    void   save(const QColor &c);
}

class ObjectBrowser : public QWidget
{
    Q_OBJECT
public:
    explicit ObjectBrowser(QWidget *parent = nullptr);

    void setConnectionLabel(const QString &label);
    /* PostgreSQL only: how the browser reaches a database other than the
     * one it's already connected to, for the multi-database tree below —
     * a Postgres connection can't query a sibling database at all, so
     * showing "every database" means lazily opening a side connection to
     * whichever one the user actually expands. The callback signature
     * mirrors ConnectionTab::connectionFor(): empty/primary database in,
     * m_conn back; anything else, an existing or freshly-opened side
     * connection, or nullptr with *error set. Never called for MySQL/
     * SQLite (loadDatabases() doesn't build the extra tree level there). */
    void setConnectionResolver(
        std::function<IDbConnection *(const QString &database, QString *error)> resolver);
    /* currentDb: the schema to auto-select/expand (matches defaultDb()) —
     * for MySQL/SQLite this is also the database itself. primaryDb: the
     * tab's actual connected *database* (PostgreSQL only; ignored/unused
     * for MySQL/SQLite, where there's no separate database-vs-schema
     * split) — needed so the multi-database tree knows which top-level
     * database node is "already connected" (via `conn` directly) versus
     * one that needs a side connection resolved on expand. */
    void loadDatabases(IDbConnection *conn, const QString &currentDb,
                       const QString &primaryDb = {});

    /* [db, table] of the currently selected table item, else empty */
    QStringList currentTableInfo() const;

    void collapseTree();   /* Edit ▸ Collapse All in Object Browser */
    /* PostgreSQL multi-database tree, headless-test hook: expand the
     * top-level database node named `name` exactly as a real click on its
     * arrow would (QTreeWidget::expandItem() triggers the same
     * itemExpanded signal onItemExpanded() is already connected to) —
     * lets a screenshot selftest exercise the lazy side-connection load
     * without simulating mouse input. */
    void expandTopLevelDatabase(const QString &name);

signals:
    void databaseActivated(const QString &db);      /* double click → USE */
    void tableActivated(const QString &db, const QString &table); /* → SELECT */
    void dropTableRequested(const QString &db, const QString &table);
    void truncateTableRequested(const QString &db, const QString &table);
    void createTableRequested(const QString &db);
    void alterTableRequested(const QString &db, const QString &table);
    void renameTableRequested(const QString &db, const QString &table);
    void copyTableRequested(const QString &db, const QString &table);
    void manageIndexesRequested(const QString &db, const QString &table);
    void manageForeignKeysRequested(const QString &db, const QString &table);
    void copyDatabaseRequested(const QString &db);
    void importCsvRequested(const QString &db, const QString &table);
    void importXmlRequested(const QString &db, const QString &table);
    void exportTableRequested(const QString &db, const QString &table);
    void dumpDatabaseRequested(const QString &db);
    /* schema objects — objType is the SQL keyword: VIEW / PROCEDURE / FUNCTION
     * / TRIGGER / EVENT */
    void createObjectRequested(const QString &db, const QString &objType);
    void alterObjectRequested(const QString &db, const QString &objType,
                              const QString &name);
    void dropObjectRequested(const QString &db, const QString &objType,
                             const QString &name);
    void dropDatabaseRequested(const QString &db);
    void truncateDatabaseRequested(const QString &db);
    void emptyDatabaseRequested(const QString &db);
    void alterDatabaseRequested(const QString &db);
    void statusMessage(const QString &text);
    /* PostgreSQL multi-database tree only: user asked to open a *different*
     * physical database (not the one this tab is connected to) as its own
     * connection tab — the browser can show any database's structure, but
     * modifying/opening table data always goes through a tab whose own
     * connection is scoped to the right database, not this one's. */
    void openDatabaseInNewTabRequested(const QString &database);

private slots:
    void onItemExpanded(QTreeWidgetItem *item);
    void applyFilter(const QString &text);

private:
    void copyCreateTable(const QString &db, const QString &table, const QString &physDb);
    void copyColumnNames(QTreeWidgetItem *tableItem);

    enum ItemRole { RoleKind = Qt::UserRole + 1, RoleName };
    enum Kind { KindConnection, KindDatabase, KindFolder, KindTable };

    /* PostgreSQL only: resolves a physical-database tree node to the
     * connection it should use (see setConnectionResolver()'s doc comment) */
    IDbConnection *connFor(const QString &physDb, QString *error = nullptr) const;

    QLabel        *m_filterLabel = nullptr;
    QLineEdit     *m_filter = nullptr;
    QTreeWidget   *m_tree   = nullptr;
    IDbConnection *m_conn   = nullptr;
    QString        m_primaryDatabase;   /* PostgreSQL: this tab's own connected db */
    std::function<IDbConnection *(const QString &, QString *)> m_resolveConn;
};
