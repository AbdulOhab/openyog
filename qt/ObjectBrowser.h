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
class QMenu;

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
     * one that needs a side connection resolved on expand. autoDrill:
     * when currentDb matches, also select/expand it and jump straight
     * into its Tables folder (SQLyog's "show me my tables" reflex on a
     * fresh connect/refresh) — pass false when the tree is already being
     * browsed and this call is just a database switch, so the top-level
     * node only reveals its own schema list instead of also diving two
     * levels deeper than an ordinary expand-arrow click would. */
    void loadDatabases(IDbConnection *conn, const QString &currentDb,
                       const QString &primaryDb = {}, bool autoDrill = true);

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
    /* headless-test hook: select (but don't expand) the tree item found by
     * following `path` — a '/'-separated chain of item texts from the top
     * level down (e.g. "postgres/public/Tables") — for reproducing/
     * verifying selection-styling issues without simulating mouse input */
    void selectTreeItem(const QString &path);
    /* headless-test hook: same path walk as selectTreeItem(), but then
     * synthesizes a real mouse press+release on the item — exercises the
     * itemClicked handlers (e.g. single-click-activates-a-schema) exactly
     * as a user click would, which a programmatic setCurrentItem() does
     * not (no itemClicked signal fires for it). */
    void clickTreeItem(const QString &path);
    /* same path walk, synthesizing a real double-click (press+release+
     * doubleclick+release) — the only way this app currently opens a
     * table's data from the tree, so a single-click test hook alone can't
     * exercise it */
    void doubleClickTreeItem(const QString &path);
    /* headless-test hook: same path walk, but collapses the found item
     * (QTreeWidgetItem::setExpanded(false)) — for reproducing collapse/
     * re-click interaction bugs without simulating mouse input on the
     * (possibly invisible, see the Theme.cpp ::branch comment) arrow */
    void collapseTreeItem(const QString &path);
    /* same path walk, expanding instead (QTreeWidgetItem::setExpanded(true),
     * which — unlike a real arrow click — does trigger itemExpanded, so
     * lazy children populate exactly as they would for a user) */
    void expandTreeItem(const QString &path);
    /* same path walk, returns the found item's subtree as indented text
     * (empty when the path doesn't resolve) — lets a selftest assert on
     * what actually got listed under a folder, not just screenshot it */
    QStringList dumpSubtree(const QString &path) const;
    /* headless-test hook: same path walk, then builds the exact right-click
     * context menu that item would show — without opening it (QMenu::exec()
     * blocks in a nested event loop, same problem the modal guard dialogs
     * had) — and returns the item texts in order ("---" for a separator,
     * "› " prefix for a submenu's own items, flattened one level deep,
     * enough for "Create Object"). Lets a selftest compare what menu a
     * given tree node offers across drivers side by side, not just
     * eyeball a screenshot. */
    QStringList contextMenuItemsForTest(const QString &path);

signals:
    void databaseActivated(const QString &db);      /* double click → USE */
    void tableActivated(const QString &db, const QString &table,
                        const QString &physDb = {});   /* → SELECT; physDb routes
                        the data grid at the right connection (PostgreSQL's
                        multi-database tree: a table under a non-primary
                        database must load via that database's side
                        connection, not m_conn) */
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
    /* PostgreSQL multi-database tree only: user asked to make a *different*
     * physical database this tab's own primary connection — this tab's
     * whole tree, toolbar and every action then work on `database` exactly
     * as if it had been the one connected to from the start (a Postgres
     * connection can't switch databases in place at the protocol level,
     * so ConnectionTab::switchDatabase() actually opens a fresh connection
     * underneath and swaps it in, keeping the *previous* one cached as a
     * side connection so switching back doesn't reconnect). Emitted by
     * double-click and by this same node's own "Switch to `db`" menu
     * entry; it's the default because staying in one tab is what's
     * normally wanted, not a second tab to compare two databases side by
     * side. The toolbar combo plays no part in switching databases — it
     * only ever lists the current one's schemas. */
    void switchDatabaseRequested(const QString &database);
    /* explicit alternative to the above, still offered from the right-click
     * menu: open `database` as a genuinely separate tab instead of taking
     * over this one — for actually comparing two databases side by side. */
    void openDatabaseInNewTabRequested(const QString &database);

private slots:
    void onItemExpanded(QTreeWidgetItem *item);
    void applyFilter(const QString &text);

private:
    void copyCreateTable(const QString &db, const QString &table, const QString &physDb);
    void copyColumnNames(QTreeWidgetItem *tableItem);
    /* builds the right-click context menu for one tree item — factored out
     * of the customContextMenuRequested handler so contextMenuItemsForTest()
     * can build the same menu without executing it */
    void populateContextMenu(QMenu &menu, QTreeWidgetItem *item);
    /* the "Create Table…"/"Create Object"/"Copy|Alter|Backup|Empty|
     * Truncate|Drop Database…" block — shared by a KDatabase node (every
     * driver) and, for PostgreSQL, a KPgDatabase node that IS the tab's
     * own primary database (safe: same m_conn, no cross-connection
     * routing needed) */
    void populateDatabaseMenu(QMenu &menu, const QString &db);

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
