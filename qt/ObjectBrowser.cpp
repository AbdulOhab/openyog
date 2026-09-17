#include "ObjectBrowser.h"
#include "Icons.h"
#include "db/IDbConnection.h"

#include "CommonHelper.h" /* port shim: wyString */
#include "wyIni.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QMenu>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <functional>

namespace {
QString settingsIniPath()
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    dir.mkpath(".");
    return dir.filePath("OpenYog.ini");
}
} // namespace

QColor ObjectBrowserColor::load()
{
    wyString value;
    wyIni::IniGetString("UserInterface", "browsercolor", "", &value, settingsIniPath().toUtf8());
    const QString v = QString::fromUtf8(value.GetString());
    return v.isEmpty() ? QColor() : QColor(v);
}

void ObjectBrowserColor::save(const QColor &c)
{
    wyIni::IniWriteString("UserInterface", "browsercolor",
                          c.isValid() ? c.name().toUtf8() : QByteArray(),
                          settingsIniPath().toUtf8());
}

namespace {
constexpr int KConnection = 1001;
constexpr int KDatabase = 1002;
constexpr int KFolder = 1003;
constexpr int KTable = 1004;
constexpr int KLeaf = 1005; /* view / proc / func / trigger / event / column */
/* PostgreSQL only: a physical database on the server (parent of KDatabase,
 * which for Postgres means a *schema* — see PostgresConnection.h). Never
 * created for MySQL/SQLite, where KDatabase already means a real database
 * and one connection already sees every one of them. */
constexpr int KPgDatabase = 1006;

/* which physical database a tree item's own subtree belongs to — empty
 * everywhere for MySQL/SQLite (meaning "the tab's one connection"); for
 * Postgres, sets which side connection (see ObjectBrowser::connFor())
 * every descendant of a KPgDatabase node should use. Kept as its own role
 * rather than folded into UserRole+1 ("extra") since KDatabase/KFolder/
 * KTable already use that slot for the schema name / table name. */
constexpr int RolePhysDb = Qt::UserRole + 3;

/* schema-object folder name → the SQL keyword for CREATE/ALTER/DROP … , or
 * empty if the folder isn't a routine/view/trigger/event folder */
QString folderObjType(const QString &folder)
{
    if(folder == QStringLiteral("Views"))
        return QStringLiteral("VIEW");
    if(folder == QStringLiteral("Stored Procs"))
        return QStringLiteral("PROCEDURE");
    if(folder == QStringLiteral("Functions"))
        return QStringLiteral("FUNCTION");
    if(folder == QStringLiteral("Triggers"))
        return QStringLiteral("TRIGGER");
    if(folder == QStringLiteral("Events"))
        return QStringLiteral("EVENT");
    return {};
}

/* extra = db name for KDatabase/KFolder/KTable; the folder's leaf query lives
 * on the KFolder item's text(0). physDb propagates RolePhysDb down from the
 * parent — pass the parent's own physDb (or its own name, for a
 * KPgDatabase item, whose "physDb" is itself). */
QTreeWidgetItem *makeItem(int kind, const QString &name, const QString &extra = {},
                          const QString &physDb = {})
{
    auto *item = new QTreeWidgetItem;
    item->setText(0, name);
    item->setData(0, Qt::UserRole, kind);
    item->setData(0, Qt::UserRole + 1, extra);
    item->setData(0, RolePhysDb, physDb);
    if(kind == KPgDatabase || kind == KDatabase || kind == KFolder || kind == KTable)
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    return item;
}

/* append each name as a KLeaf child */
void fillLeaves(QTreeWidgetItem *parent, const QStringList &names, const QString &icon,
                const QString &physDb = {})
{
    for(const QString &name : names) {
        auto *leaf = makeItem(KLeaf, name, {}, physDb);
        leaf->setIcon(0, Icons::get(icon));
        parent->addChild(leaf);
    }
}
} // namespace

ObjectBrowser::ObjectBrowser(QWidget *parent) : QWidget(parent)
{
    m_filterLabel = new QLabel(this);
    m_filterLabel->setObjectName(QStringLiteral("obFilterLabel"));
    m_filter = new QLineEdit(this);
    m_filter->setClearButtonEnabled(true);
    m_filter->setPlaceholderText(QStringLiteral("Filter (Ctrl+Shift+B)"));
    connect(m_filter, &QLineEdit::textChanged, this, &ObjectBrowser::applyFilter);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setIndentation(14);         /* spec §4 */
    m_tree->setIconSize(QSize(16, 16)); /* spec §4: 16x16 image list */
    m_tree->setUniformRowHeights(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    if(const QColor c = ObjectBrowserColor::load(); c.isValid())
        m_tree->setStyleSheet(
            QStringLiteral("QTreeWidget::item:selected{background:%1}").arg(c.name()));
    connect(m_tree, &QTreeWidget::itemExpanded, this, &ObjectBrowser::onItemExpanded);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = m_tree->itemAt(pos);
        if(!item)
            return;
        QMenu menu(this);
        populateContextMenu(menu, item);
        if(!menu.isEmpty())
            menu.exec(m_tree->viewport()->mapToGlobal(pos));
    });

    /* single click on a schema/database node makes it current too (same
     * databaseActivated the double-click below emits) — the owner's ask:
     * after switching databases the tree stops at the schema list, and
     * clicking "public" there should show up in the toolbar combo right
     * away instead of waiting for a double-click. Skipped for a schema
     * under a *different* physical database (can't SET search_path to it
     * from this tab's connection — expanding it via its side connection
     * is browsing only) and for SQLite, whose single "main" node has
     * nothing switchable and would just surface a USE syntax error. */
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        const int kind = item->data(0, Qt::UserRole).toInt();
        /* Info tab: any real schema object, single click — a KTable (under
         * "Tables") or a KLeaf under one of the other object folders
         * (Views/Stored Procs/Functions/Triggers/Events), not a Columns/
         * Indexes sub-leaf (folderObjType() is empty for those, so this
         * naturally excludes them without a separate check). */
        if(kind == KTable) {
            emit objectSelected(item->data(0, Qt::UserRole + 1).toString(), QStringLiteral("TABLE"),
                                item->text(0), item->data(0, RolePhysDb).toString());
        } else if(kind == KLeaf && item->parent()) {
            const QString objType = folderObjType(item->parent()->text(0));
            if(!objType.isEmpty())
                emit objectSelected(item->parent()->data(0, Qt::UserRole + 1).toString(), objType,
                                    item->text(0), item->data(0, RolePhysDb).toString());
        }

        if(kind != KDatabase || m_conn->driverType() == DriverType::Sqlite)
            return;
        const QString physDb = item->data(0, RolePhysDb).toString();
        if(!physDb.isEmpty() && physDb != m_primaryDatabase)
            return;
        emit databaseActivated(item->data(0, Qt::UserRole + 1).toString());
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        const int kind = item->data(0, Qt::UserRole).toInt();
        const QString physDb = item->data(0, RolePhysDb).toString();
        const bool foreign = !physDb.isEmpty() && physDb != m_primaryDatabase;
        /* upstream ObjectBrowser.cpp's WM_LBUTTONDBLCLK: with GetTextOnDBClick
         * (default on) double-clicking a node's label drops its name —
         * already quoted for the dialect, CQueryObject::InsertNodeText's
         * job — at the active editor's caret and focuses the editor; the
         * tree keeps every other gesture off that path (table → data grid,
         * object leaf → ALTER, database → USE), reachable via F11 / the
         * right-click menu / single click. Turning the key to 0 restores
         * those gestures on double-click, which is upstream's own alternate
         * branch (ShowTable). */
        const bool insertOnDbl = wyIni::IniGetInt("UserInterface", "GetTextOnDBClick", 1,
                                                  settingsIniPath().toUtf8()) != 0;
        /* the database node itself is the switch gesture (double-
         * click = make this tab's connection that database) */
        if(kind == KPgDatabase)
            emit switchDatabaseRequested(item->text(0));
        else if(insertOnDbl && (kind == KDatabase || kind == KTable || kind == KLeaf)) {
            QString name = item->text(0);
            if(m_conn->driverType() == DriverType::Mysql)
                name = QLatin1Char('`') + name + QLatin1Char('`');
            else
                name = QLatin1Char('"') + name + QLatin1Char('"');
            emit insertNameRequested(name);
        } else if(kind == KTable)
            /* open the clicked table's data — on EITHER database.
             * The table under a non-primary database used to route
             * here too and switch the whole connection instead,
             * throwing away the user's tree state (the reported
             * "clicked a table in half26, everything collapsed"
             * bug); it now opens through that database's side
             * connection, like the context menu's Open Table Data.
             * physDb routes the data grid to the right connection
             * (empty on MySQL/SQLite → m_conn). */
            emit tableActivated(item->data(0, Qt::UserRole + 1).toString(), item->text(0), physDb);
        else if(foreign)
            return; /* other foreign items: double-click just
                       expands in place — browsing, not switching */
        else if(kind == KDatabase)
            emit databaseActivated(item->data(0, Qt::UserRole + 1).toString());
        else if(kind == KLeaf && item->parent() &&
                !folderObjType(item->parent()->text(0)).isEmpty())
            emit alterObjectRequested(item->parent()->data(0, Qt::UserRole + 1).toString(),
                                      folderObjType(item->parent()->text(0)), item->text(0));
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_filterLabel);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree, 1);
}

void ObjectBrowser::populateContextMenu(QMenu &menu, QTreeWidgetItem *item)
{
    const int kind = item->data(0, Qt::UserRole).toInt();
    const QString physDb = item->data(0, RolePhysDb).toString();
    /* an item under a *different* physical database than this tab's
     * own connection (PostgreSQL's multi-database tree only) — its
     * structure can be browsed (connFor() opens/reuses a side
     * connection for that), but every mutating action below assumes
     * m_conn, the tab's own connection, so those stay unavailable
     * here; "Connect in New Tab" is the way to actually work with it. */
    const bool foreign = !physDb.isEmpty() && physDb != m_primaryDatabase;

    if(kind == KPgDatabase) {
        const QString database = item->text(0);
        /* this node LOOKS like a MySQL/SQLite database node (same icon,
         * same visual tree depth) but PostgreSQL wraps an extra "physical
         * database" level around its schemas — so it used to get only
         * Switch/Connect/Refresh below, never the rich Create Table/Alter
         * Database/… menu a MySQL/SQLite database gets, even when this IS
         * the tab's own already-connected database and every one of those
         * actions is perfectly safe to run through m_conn directly (no
         * cross-connection routing needed — that's only a problem for a
         * *different*, foreign physical database, handled below). Reported
         * by the owner as "SQLite/Postgres show less menu than MySQL". */
        if(database == m_primaryDatabase) {
            /* Postgres tables live in a schema, not "the database"
             * directly, so these route to a schema name: prefer "public"
             * (the default every fresh Postgres database starts with, and
             * the same fallback ConnectionTab::defaultDb() itself uses),
             * else whichever schema this database actually has. This
             * node's schema children are populated eagerly for the
             * primary database (see loadDatabases()), not lazily on
             * expand, so childCount() is already accurate here. */
            QString schema = QStringLiteral("public");
            bool hasPublic = false;
            for(int i = 0; i < item->childCount(); ++i)
                if(item->child(i)->text(0) == schema) {
                    hasPublic = true;
                    break;
                }
            if(!hasPublic && item->childCount() > 0)
                schema = item->child(0)->text(0);
            populateDatabaseMenu(menu, schema);
            menu.addSeparator();
        }
        menu.addAction(QStringLiteral("&Switch to `%1`").arg(database), this,
                       [this, database] { emit switchDatabaseRequested(database); });
        menu.addAction(QStringLiteral("Connect to `%1` in New &Tab…").arg(database), this,
                       [this, database] { emit openDatabaseInNewTabRequested(database); });
        menu.addAction(QStringLiteral("Re&fresh Node"), this, [this, item] {
            item->takeChildren();
            onItemExpanded(item);
            item->setExpanded(true);
        });
    } else if(foreign) {
        /* reduced menu for anything under a non-primary database:
         * browsing already works (the tree got here via connFor()),
         * but no action below this point is safe to route through
         * m_conn, so only offer what doesn't need it — Switch fixes
         * that by making `physDb` the primary connection instead */
        menu.addAction(QStringLiteral("&Switch to `%1`").arg(physDb), this,
                       [this, physDb] { emit switchDatabaseRequested(physDb); });
        menu.addAction(QStringLiteral("Connect to `%1` in New &Tab…").arg(physDb), this,
                       [this, physDb] { emit openDatabaseInNewTabRequested(physDb); });
        if(kind == KTable)
            /* viewing data is safe on a foreign table — the data grid
             * gets that database's own side connection (tableActivated
             * carries physDb), unlike the Alter/Index/FK actions that
             * stay primary-only below */
            menu.addAction(QStringLiteral("Open Table &Data"), this, [this, item] {
                emit tableActivated(item->data(0, Qt::UserRole + 1).toString(), item->text(0),
                                    item->data(0, RolePhysDb).toString());
            });
        if(kind == KDatabase || kind == KFolder || kind == KTable)
            menu.addAction(QStringLiteral("Re&fresh Node"), this, [this, item] {
                item->takeChildren();
                onItemExpanded(item);
                item->setExpanded(true);
            });
    } else if(kind == KDatabase) {
        populateDatabaseMenu(menu, item->text(0));
    } else if(kind == KFolder && item->text(0) == QStringLiteral("Tables")) {
        const QString db = item->data(0, Qt::UserRole + 1).toString();
        menu.addAction(QStringLiteral("Create &Table…"), this,
                       [this, db] { emit createTableRequested(db); });
    } else if(kind == KFolder && !folderObjType(item->text(0)).isEmpty()) {
        const QString db = item->data(0, Qt::UserRole + 1).toString();
        const QString t = folderObjType(item->text(0));
        menu.addAction(QStringLiteral("&Create %1…").arg(t.at(0) + t.mid(1).toLower()), this,
                       [this, db, t] { emit createObjectRequested(db, t); });
    } else if(kind == KLeaf && item->parent() &&
              !folderObjType(item->parent()->text(0)).isEmpty()) {
        const QString db = item->parent()->data(0, Qt::UserRole + 1).toString();
        const QString t = folderObjType(item->parent()->text(0));
        const QString name = item->text(0);
        const QString nice = t.at(0) + t.mid(1).toLower();
        menu.addAction(QStringLiteral("&Alter %1…").arg(nice), this,
                       [this, db, t, name] { emit alterObjectRequested(db, t, name); });
        menu.addAction(QStringLiteral("&Drop %1…").arg(nice), this,
                       [this, db, t, name] { emit dropObjectRequested(db, t, name); });
    } else if(kind == KTable) {
        const QString db = item->data(0, Qt::UserRole + 1).toString();
        const QString table = item->text(0);
        menu.addAction(QStringLiteral("Open Table &Data"), this,
                       [this, db, table, physDb] { emit tableActivated(db, table, physDb); });
        menu.addAction(QStringLiteral("&Alter Table…"), this,
                       [this, db, table] { emit alterTableRequested(db, table); });
        menu.addAction(QStringLiteral("&Manage Indexes…"), this,
                       [this, db, table] { emit manageIndexesRequested(db, table); });
        menu.addAction(QStringLiteral("&Foreign Keys…"), this,
                       [this, db, table] { emit manageForeignKeysRequested(db, table); });
        menu.addAction(QStringLiteral("&Rename Table…"), this,
                       [this, db, table] { emit renameTableRequested(db, table); });
        menu.addAction(QStringLiteral("D&uplicate Table…"), this,
                       [this, db, table] { emit copyTableRequested(db, table); });
        menu.addAction(QStringLiteral("&Import CSV…"), this,
                       [this, db, table] { emit importCsvRequested(db, table); });
        menu.addAction(QStringLiteral("Import &XML…"), this,
                       [this, db, table] { emit importXmlRequested(db, table); });
        menu.addAction(QStringLiteral("&Export Table Data As…"), this,
                       [this, db, table] { emit exportTableRequested(db, table); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Create &Table…"), this,
                       [this, db] { emit createTableRequested(db); });
        menu.addAction(QStringLiteral("&Drop Table…"), this,
                       [this, db, table] { emit dropTableRequested(db, table); });
        menu.addAction(QStringLiteral("&Truncate Table…"), this,
                       [this, db, table] { emit truncateTableRequested(db, table); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("&Copy CREATE Statement"), this,
                       [this, db, table, physDb] { copyCreateTable(db, table, physDb); });
        menu.addAction(QStringLiteral("Copy Column &Names"), this,
                       [this, item] { copyColumnNames(item); });
        menu.addAction(QStringLiteral("Re&fresh Node"), this, [this, item] {
            item->takeChildren();
            onItemExpanded(item);
            item->setExpanded(true);
        });
    } else if(kind == KLeaf && item->parent() &&
              item->parent()->data(0, Qt::UserRole).toInt() == KFolder &&
              item->parent()->text(0) == QStringLiteral("Columns")) {
        const QString col = item->text(0).section(QStringLiteral("  :  "), 0, 0);
        menu.addAction(QStringLiteral("&Copy Column Name"), this,
                       [col] { QApplication::clipboard()->setText(col); });
    }
}

void ObjectBrowser::populateDatabaseMenu(QMenu &menu, const QString &db)
{
    menu.addAction(QStringLiteral("Create &Table…"), this,
                   [this, db] { emit createTableRequested(db); });
    QMenu *create = menu.addMenu(QStringLiteral("&Create Object"));
    for(const auto &kw :
        {QStringLiteral("VIEW"), QStringLiteral("PROCEDURE"), QStringLiteral("FUNCTION"),
         QStringLiteral("TRIGGER"), QStringLiteral("EVENT")}) {
        const QString t = kw;
        create->addAction(t.at(0) + t.mid(1).toLower() + QStringLiteral("…"), this,
                          [this, db, t] { emit createObjectRequested(db, t); });
    }
    menu.addAction(QStringLiteral("&Copy Database…"), this,
                   [this, db] { emit copyDatabaseRequested(db); });
    menu.addAction(QStringLiteral("&Alter Database…"), this,
                   [this, db] { emit alterDatabaseRequested(db); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("&Backup Database As SQL Dump…"), this,
                   [this, db] { emit dumpDatabaseRequested(db); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("&Empty Database (truncate all tables)…"), this,
                   [this, db] { emit emptyDatabaseRequested(db); });
    menu.addAction(QStringLiteral("&Truncate Database (drop all objects)…"), this,
                   [this, db] { emit truncateDatabaseRequested(db); });
    menu.addAction(QStringLiteral("&Drop Database…"), this,
                   [this, db] { emit dropDatabaseRequested(db); });
}

void ObjectBrowser::setConnectionLabel(const QString &label)
{
    m_tree->clear();
    auto *root = makeItem(KConnection, label);
    root->setIcon(0, Icons::get(QStringLiteral("connect_16.ico")));
    m_tree->addTopLevelItem(root);
    m_tree->expandItem(root);
}

void ObjectBrowser::setConnectionResolver(
    std::function<IDbConnection *(const QString &, QString *)> resolver)
{
    m_resolveConn = std::move(resolver);
}

IDbConnection *ObjectBrowser::connFor(const QString &physDb, QString *error) const
{
    if(physDb.isEmpty() || physDb == m_primaryDatabase)
        return m_conn;
    return m_resolveConn ? m_resolveConn(physDb, error) : nullptr;
}

void ObjectBrowser::loadDatabases(IDbConnection *conn, const QString &currentDb,
                                  const QString &primaryDb, bool autoDrill)
{
    m_conn = conn;
    m_primaryDatabase = primaryDb;
    m_filterLabel->setText(QStringLiteral("Filter tables in %1")
                               .arg(currentDb.isEmpty() ? QStringLiteral("*") : currentDb));
    QTreeWidgetItem *root = m_tree->topLevelItem(0);
    if(!root || !m_conn)
        return;

    root->takeChildren();

    /* schema-level population, shared by both branches below: fills `dbItem`
     * (a KDatabase node) with the standard six folders, then — if it's the
     * one matching currentDb — selects/expands it and opens its Tables
     * folder straight away, like SQLyog. `physDb` is what every descendant
     * inherits (empty for MySQL/SQLite, the owning physical database for
     * Postgres). */
    const auto populateSchema = [&](QTreeWidgetItem *dbItem, const QString &dbName,
                                    const QString &physDb) {
        if(autoDrill && currentDb == dbName) {
            dbItem->setSelected(true);
            dbItem->setExpanded(true);
            onItemExpanded(dbItem);
            if(dbItem->childCount() > 0) {
                QTreeWidgetItem *tablesFolder = dbItem->child(0);
                tablesFolder->setExpanded(true);
                onItemExpanded(tablesFolder);
            }
        }
    };

    if(m_conn->driverType() == DriverType::Postgres) {
        /* one physical-database level, then schemas under the one that's
         * already connected (via m_conn) — others stay unpopulated until
         * the user actually expands them (onItemExpanded(KPgDatabase)) */
        for(const QString &physDb : m_conn->listPhysicalDatabases()) {
            auto *pd = makeItem(KPgDatabase, physDb, {}, physDb);
            pd->setIcon(0, Icons::get(QStringLiteral("database.ico")));
            root->addChild(pd);
            if(physDb == m_primaryDatabase) {
                /* populate children BEFORE setExpanded(true) below — that
                 * call emits itemExpanded synchronously (same thread,
                 * direct connection), which would otherwise re-enter
                 * onItemExpanded() while childCount() is still 0 and have
                 * IT populate the schema list too, via the exact same
                 * m_conn->listDatabases() call this loop already makes —
                 * producing every schema twice */
                for(const QString &schema : m_conn->listDatabases()) {
                    auto *db = makeItem(KDatabase, schema, schema, physDb);
                    db->setIcon(0, Icons::get(QStringLiteral("database.ico")));
                    pd->addChild(db);
                    populateSchema(db, schema, physDb);
                }
                pd->setExpanded(true);
            }
        }
    } else {
        for(const QString &dbName : m_conn->listDatabases()) {
            auto *db = makeItem(KDatabase, dbName, dbName); /* carry db name */
            db->setIcon(0, Icons::get(QStringLiteral("database.ico")));
            root->addChild(db);
            populateSchema(db, dbName, {});
        }
    }
    m_tree->expandItem(root);
}

QStringList ObjectBrowser::currentTableInfo() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if(!item || item->data(0, Qt::UserRole).toInt() != KTable)
        return {};
    /* a table under a different physical database than this tab's own
     * connection (PostgreSQL's multi-database tree) — main-menu Table
     * actions all route through m_conn, so pretend nothing is selected
     * rather than let them act on the wrong database */
    const QString physDb = item->data(0, RolePhysDb).toString();
    if(!physDb.isEmpty() && physDb != m_primaryDatabase)
        return {};
    return {item->data(0, Qt::UserRole + 1).toString(), item->text(0)};
}

void ObjectBrowser::onItemExpanded(QTreeWidgetItem *item)
{
    const int kind = item->data(0, Qt::UserRole).toInt();
    if(!m_conn || item->childCount() > 0)
        return; /* already populated, or not connected */

    const QString db = item->data(0, Qt::UserRole + 1).toString();
    const QString physDb = item->data(0, RolePhysDb).toString();

    if(kind == KPgDatabase) {
        /* only reached for a database OTHER than the primary one — that
         * one's schemas are already populated by loadDatabases() itself,
         * so childCount() > 0 short-circuits above before we get here.
         * physDb == this item's own name (see loadDatabases()). connFor()
         * opens a brand-new network connection when physDb isn't already
         * cached from earlier browsing — a real, blocking round trip on
         * the GUI thread with nothing to show for it otherwise, hence the
         * wait cursor (matches switchDatabaseRequested()'s own connect,
         * which this can lead into right after if the user then clicks
         * "Switch" — that one will already be instant, reusing this
         * connection instead of opening a second one). */
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QString error;
        IDbConnection *c = connFor(physDb, &error);
        QApplication::restoreOverrideCursor();
        if(!c) {
            auto *l = makeItem(KLeaf, QStringLiteral("(connection failed: %1)").arg(error));
            l->setDisabled(true);
            item->addChild(l);
            return;
        }
        for(const QString &schema : c->listDatabases()) {
            auto *dbItem = makeItem(KDatabase, schema, schema, physDb);
            dbItem->setIcon(0, Icons::get(QStringLiteral("database.ico")));
            item->addChild(dbItem);
        }
        if(item->childCount() == 0) {
            auto *l = makeItem(KLeaf, QStringLiteral("(no schemas)"));
            l->setDisabled(true);
            item->addChild(l);
        }
        return;
    }

    if(kind == KDatabase) {
        /* SQLyog shows these six folders under every database, each with
         * its own icon (not a generic folder glyph) */
        static const QList<QPair<QString, QString>> kFolders = {
            {QStringLiteral("Tables"), QStringLiteral("table.ico")},
            {QStringLiteral("Views"), QStringLiteral("view.ico")},
            {QStringLiteral("Stored Procs"), QStringLiteral("process.ico")},
            {QStringLiteral("Functions"), QStringLiteral("function.ico")},
            {QStringLiteral("Triggers"), QStringLiteral("trigger.ico")},
            {QStringLiteral("Events"), QStringLiteral("event.ico")},
        };
        for(const auto &[f, icon] : kFolders) {
            auto *folder = makeItem(KFolder, f, db, physDb);
            folder->setIcon(0, Icons::get(icon));
            item->addChild(folder);
        }
        return;
    }

    if(kind == KTable) {
        /* Columns + Indexes sub-folders (table name on +2) — matches
         * upstream ObjectBrowser.cpp, where TXT_COLUMNS and TXT_INDEXES are
         * both lazy-loaded folder nodes under a table, not columns printed
         * inline. Foreign Keys has no upstream tree node at all, and
         * Triggers is a database-level folder only there — managing a
         * table's FKs/triggers stays a dialog (F7/F10) or the
         * database-level Triggers folder, not a redundant per-table copy. */
        static const QList<QPair<QString, QString>> kSubFolders = {
            {QStringLiteral("Columns"), QStringLiteral("column.ico")},
            {QStringLiteral("Indexes"), QStringLiteral("index.ico")},
        };
        for(const auto &[sub, icon] : kSubFolders) {
            auto *f = makeItem(KFolder, sub, db, physDb);
            f->setData(0, Qt::UserRole + 2, item->text(0));
            f->setIcon(0, Icons::get(icon));
            item->addChild(f);
        }
        return;
    }

    if(kind != KFolder)
        return;

    QString connError;
    IDbConnection *c = connFor(physDb, &connError);
    if(!c) {
        auto *l = makeItem(KLeaf, QStringLiteral("(connection failed: %1)").arg(connError));
        l->setDisabled(true);
        item->addChild(l);
        return;
    }

    const QString folder = item->text(0);

    /* table-scoped Columns / Indexes folder */
    if(const QString tbl = item->data(0, Qt::UserRole + 2).toString(); !tbl.isEmpty()) {
        const auto add = [&](const QString &text, const QString &icon) {
            auto *l = makeItem(KLeaf, text, {}, physDb);
            l->setIcon(0, Icons::get(icon));
            item->addChild(l);
        };
        if(folder == QStringLiteral("Columns")) {
            for(const QStringList &row : c->listColumns(db, tbl).rows)
                add(QStringLiteral("%1  :  %2").arg(row.value(0), row.value(1)),
                    QStringLiteral("column.ico"));
        } else if(folder == QStringLiteral("Indexes")) {
            const DbResultSet rs = c->listIndexes(db, tbl);
            QString curName;
            QStringList curCols;
            bool curUnique = false;
            const auto flush = [&] {
                if(curName.isEmpty())
                    return;
                add(QStringLiteral("%1  %2(%3)")
                        .arg(curName, curUnique ? QStringLiteral("UNIQUE ") : QString(),
                             curCols.join(QStringLiteral(", "))),
                    QStringLiteral("altertable.ico"));
            };
            for(const QStringList &row : rs.rows) {
                const QString name = row.value(2);
                if(name != curName) {
                    flush();
                    curName = name;
                    curCols.clear();
                    curUnique = row.value(1) == QStringLiteral("0");
                }
                if(!row.value(4).isEmpty())
                    curCols << row.value(4);
            }
            flush();
        }
        if(item->childCount() == 0) {
            auto *l = makeItem(KLeaf, QStringLiteral("(none)"));
            l->setDisabled(true);
            item->addChild(l);
        }
        return;
    }
    if(folder == QStringLiteral("Tables")) {
        for(const QString &t : c->listTables(db, QStringLiteral("BASE TABLE"))) {
            auto *ti = makeItem(KTable, t, db, physDb);
            ti->setIcon(0, Icons::get(QStringLiteral("table.ico")));
            item->addChild(ti);
        }
    } else if(folder == QStringLiteral("Views")) {
        fillLeaves(item, c->listTables(db, QStringLiteral("VIEW")), QStringLiteral("alterview.ico"),
                   physDb);
    } else if(folder == QStringLiteral("Stored Procs")) {
        QStringList names;
        for(const QStringList &row : c->listRoutines(db).rows)
            if(row.value(1) == QStringLiteral("PROCEDURE"))
                names << row.value(0);
        fillLeaves(item, names, QStringLiteral("altersp.ico"), physDb);
    } else if(folder == QStringLiteral("Functions")) {
        QStringList names;
        for(const QStringList &row : c->listRoutines(db).rows)
            if(row.value(1) == QStringLiteral("FUNCTION"))
                names << row.value(0);
        fillLeaves(item, names, QStringLiteral("alterfunction.ico"), physDb);
    } else if(folder == QStringLiteral("Triggers")) {
        fillLeaves(item, c->listTriggers(db), QStringLiteral("altertrigger.ico"), physDb);
    } else if(folder == QStringLiteral("Events")) {
        fillLeaves(item, c->listEvents(db), QStringLiteral("alterevent.ico"), physDb);
    }
}

void ObjectBrowser::collapseTree()
{
    m_tree->collapseAll();
    if(QTreeWidgetItem *root = m_tree->topLevelItem(0))
        root->setExpanded(true); /* keep the connection node open */
}

void ObjectBrowser::expandTopLevelDatabase(const QString &name)
{
    QTreeWidgetItem *root = m_tree->topLevelItem(0);
    if(!root)
        return;
    for(int i = 0; i < root->childCount(); ++i) {
        QTreeWidgetItem *child = root->child(i);
        if(child->text(0) == name) {
            m_tree->expandItem(child);
            return;
        }
    }
}

void ObjectBrowser::selectTreeItem(const QString &path)
{
    QTreeWidgetItem *cur = m_tree->topLevelItem(0);
    if(!cur)
        return;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for(const QString &part : parts) {
        QTreeWidgetItem *next = nullptr;
        for(int i = 0; i < cur->childCount(); ++i)
            if(cur->child(i)->text(0) == part) {
                next = cur->child(i);
                break;
            }
        if(!next)
            return;
        cur = next;
    }
    m_tree->setCurrentItem(cur);
    cur->setSelected(true);
}

void ObjectBrowser::clickTreeItem(const QString &path)
{
    QTreeWidgetItem *cur = m_tree->topLevelItem(0);
    if(!cur)
        return;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for(const QString &part : parts) {
        QTreeWidgetItem *next = nullptr;
        for(int i = 0; i < cur->childCount(); ++i)
            if(cur->child(i)->text(0) == part) {
                next = cur->child(i);
                break;
            }
        if(!next)
            return;
        cur = next;
    }
    m_tree->setCurrentItem(cur);
    /* bring the row into view first — a click synthesized at a point that
     * lies outside the (possibly scrolled or not-yet-laid-out) viewport is
     * silently dropped by Qt, with no itemClicked to show for it */
    m_tree->scrollToItem(cur, QAbstractItemView::PositionAtCenter);
    const QRect r = m_tree->visualItemRect(cur);
    const QPoint p = r.center();
    const QPoint global = m_tree->viewport()->mapToGlobal(p);
    QMouseEvent press(QEvent::MouseButtonPress, p, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, p, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(m_tree->viewport(), &press);
    QApplication::sendEvent(m_tree->viewport(), &release);
}

void ObjectBrowser::collapseTreeItem(const QString &path)
{
    QTreeWidgetItem *cur = m_tree->topLevelItem(0);
    if(!cur)
        return;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for(const QString &part : parts) {
        QTreeWidgetItem *next = nullptr;
        for(int i = 0; i < cur->childCount(); ++i)
            if(cur->child(i)->text(0) == part) {
                next = cur->child(i);
                break;
            }
        if(!next)
            return;
        cur = next;
    }
    cur->setExpanded(false);
}

void ObjectBrowser::doubleClickTreeItem(const QString &path)
{
    QTreeWidgetItem *cur = m_tree->topLevelItem(0);
    if(!cur)
        return;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for(const QString &part : parts) {
        QTreeWidgetItem *next = nullptr;
        for(int i = 0; i < cur->childCount(); ++i)
            if(cur->child(i)->text(0) == part) {
                next = cur->child(i);
                break;
            }
        if(!next)
            return;
        cur = next;
    }
    m_tree->setCurrentItem(cur);
    m_tree->scrollToItem(cur, QAbstractItemView::PositionAtCenter);
    const QRect r = m_tree->visualItemRect(cur);
    const QPoint p = r.center();
    const QPoint global = m_tree->viewport()->mapToGlobal(p);
    QMouseEvent press1(QEvent::MouseButtonPress, p, global, Qt::LeftButton, Qt::LeftButton,
                       Qt::NoModifier);
    QMouseEvent release1(QEvent::MouseButtonRelease, p, global, Qt::LeftButton, Qt::NoButton,
                         Qt::NoModifier);
    QMouseEvent dblClick(QEvent::MouseButtonDblClick, p, global, Qt::LeftButton, Qt::LeftButton,
                         Qt::NoModifier);
    QMouseEvent release2(QEvent::MouseButtonRelease, p, global, Qt::LeftButton, Qt::NoButton,
                         Qt::NoModifier);
    QApplication::sendEvent(m_tree->viewport(), &press1);
    QApplication::sendEvent(m_tree->viewport(), &release1);
    QApplication::sendEvent(m_tree->viewport(), &dblClick);
    QApplication::sendEvent(m_tree->viewport(), &release2);
}

void ObjectBrowser::expandTreeItem(const QString &path)
{
    QTreeWidgetItem *cur = m_tree->topLevelItem(0);
    if(!cur)
        return;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for(const QString &part : parts) {
        QTreeWidgetItem *next = nullptr;
        for(int i = 0; i < cur->childCount(); ++i)
            if(cur->child(i)->text(0) == part) {
                next = cur->child(i);
                break;
            }
        if(!next)
            return;
        cur = next;
    }
    cur->setExpanded(true);
}

QStringList ObjectBrowser::dumpSubtree(const QString &path) const
{
    QTreeWidgetItem *cur = m_tree->topLevelItem(0);
    if(!cur)
        return {};
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for(const QString &part : parts) {
        QTreeWidgetItem *next = nullptr;
        for(int i = 0; i < cur->childCount(); ++i)
            if(cur->child(i)->text(0) == part) {
                next = cur->child(i);
                break;
            }
        if(!next)
            return {}; /* unresolved path — same silence as the walkers */
        cur = next;
    }
    QStringList out;
    const std::function<void(QTreeWidgetItem *, int)> walk = [&](QTreeWidgetItem *item, int depth) {
        out << QStringLiteral("%1%2").arg(QString(depth * 2, QLatin1Char(' ')), item->text(0));
        for(int i = 0; i < item->childCount(); ++i)
            walk(item->child(i), depth + 1);
    };
    walk(cur, 0);
    return out;
}

QStringList ObjectBrowser::contextMenuItemsForTest(const QString &path)
{
    QTreeWidgetItem *cur = m_tree->topLevelItem(0);
    if(!cur)
        return {};
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for(const QString &part : parts) {
        QTreeWidgetItem *next = nullptr;
        for(int i = 0; i < cur->childCount(); ++i)
            if(cur->child(i)->text(0) == part) {
                next = cur->child(i);
                break;
            }
        if(!next)
            return {}; /* unresolved path — same silence as the walkers */
        cur = next;
    }
    QMenu menu;
    populateContextMenu(menu, cur);
    QStringList out;
    for(QAction *a : menu.actions()) {
        if(a->isSeparator()) {
            out << QStringLiteral("---");
            continue;
        }
        out << a->text();
        /* one level of submenu (e.g. "Create Object" ▸ View/Procedure/…) —
         * enough for every menu this app builds, none nest deeper */
        if(QMenu *sub = a->menu())
            for(QAction *sa : sub->actions())
                out << QStringLiteral("  › %1").arg(sa->text());
    }
    return out;
}

void ObjectBrowser::copyCreateTable(const QString &db, const QString &table, const QString &physDb)
{
    QString error;
    IDbConnection *c = connFor(physDb, &error);
    if(!c) {
        emit statusMessage(QStringLiteral("SHOW CREATE failed: %1").arg(error));
        return;
    }
    const QString ddl = c->showCreate(QStringLiteral("TABLE"), db, table, &error);
    if(ddl.isEmpty()) {
        if(!error.isEmpty())
            emit statusMessage(QStringLiteral("SHOW CREATE failed: %1").arg(error));
        return;
    }
    QApplication::clipboard()->setText(ddl + QLatin1Char(';'));
    emit statusMessage(QStringLiteral("CREATE statement for `%1` copied").arg(table));
}

void ObjectBrowser::copyColumnNames(QTreeWidgetItem *tableItem)
{
    if(!tableItem)
        return;
    if(tableItem->childCount() == 0)
        onItemExpanded(tableItem);
    /* columns live one level deeper now: table -> "Columns" folder -> leaves */
    QTreeWidgetItem *columnsFolder = nullptr;
    for(int i = 0; i < tableItem->childCount(); ++i) {
        QTreeWidgetItem *c = tableItem->child(i);
        if(c->data(0, Qt::UserRole).toInt() == KFolder && c->text(0) == QStringLiteral("Columns")) {
            columnsFolder = c;
            break;
        }
    }
    if(!columnsFolder)
        return;
    if(columnsFolder->childCount() == 0)
        onItemExpanded(columnsFolder);
    QStringList names;
    for(int i = 0; i < columnsFolder->childCount(); ++i) {
        QTreeWidgetItem *c = columnsFolder->child(i);
        if(c->data(0, Qt::UserRole).toInt() != KLeaf)
            continue;
        names << c->text(0).section(QStringLiteral("  :  "), 0, 0).trimmed();
    }
    if(!names.isEmpty()) {
        QApplication::clipboard()->setText(names.join(QStringLiteral(", ")));
        emit statusMessage(QStringLiteral("%1 column name(s) copied").arg(names.size()));
    }
}

void ObjectBrowser::applyFilter(const QString &text)
{
    /* filter the whole tree, matching SQLyog's "Filter tables" box */
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *item) {
        bool visible = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive);
        for(int i = 0; i < item->childCount(); ++i) {
            walk(item->child(i));
            if(!item->child(i)->isHidden())
                visible = true;
        }
        item->setHidden(!visible && item->parent() != nullptr);
        if(!text.isEmpty() && visible && item->childCount())
            item->setExpanded(true);
    };
    for(int i = 0; i < m_tree->topLevelItemCount(); ++i)
        walk(m_tree->topLevelItem(i));
}
