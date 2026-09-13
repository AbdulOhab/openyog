#include "ObjectBrowser.h"
#include "Icons.h"
#include "db/IDbConnection.h"

#include "CommonHelper.h"   /* port shim: wyString */
#include "wyIni.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QMenu>
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
    wyIni::IniGetString("UserInterface", "browsercolor", "", &value,
                        settingsIniPath().toUtf8());
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
constexpr int KDatabase   = 1002;
constexpr int KFolder     = 1003;
constexpr int KTable      = 1004;
constexpr int KLeaf       = 1005;   /* view / proc / func / trigger / event / column */

/* schema-object folder name → the SQL keyword for CREATE/ALTER/DROP … , or
 * empty if the folder isn't a routine/view/trigger/event folder */
QString folderObjType(const QString &folder)
{
    if(folder == QStringLiteral("Views"))       return QStringLiteral("VIEW");
    if(folder == QStringLiteral("Stored Procs")) return QStringLiteral("PROCEDURE");
    if(folder == QStringLiteral("Functions"))   return QStringLiteral("FUNCTION");
    if(folder == QStringLiteral("Triggers"))    return QStringLiteral("TRIGGER");
    if(folder == QStringLiteral("Events"))      return QStringLiteral("EVENT");
    return {};
}

/* extra = db name for KDatabase/KFolder/KTable; the folder's leaf query lives
 * on the KFolder item's text(0) */
QTreeWidgetItem *makeItem(int kind, const QString &name, const QString &extra = {})
{
    auto *item = new QTreeWidgetItem;
    item->setText(0, name);
    item->setData(0, Qt::UserRole, kind);
    item->setData(0, Qt::UserRole + 1, extra);
    if(kind == KDatabase || kind == KFolder || kind == KTable)
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    return item;
}

/* append each name as a KLeaf child */
void fillLeaves(QTreeWidgetItem *parent, const QStringList &names, const QString &icon)
{
    for(const QString &name : names) {
        auto *leaf = makeItem(KLeaf, name);
        leaf->setIcon(0, Icons::get(icon));
        parent->addChild(leaf);
    }
}
} // namespace

ObjectBrowser::ObjectBrowser(QWidget *parent)
    : QWidget(parent)
{
    m_filterLabel = new QLabel(this);
    m_filterLabel->setObjectName(QStringLiteral("obFilterLabel"));
    m_filter = new QLineEdit(this);
    m_filter->setClearButtonEnabled(true);
    m_filter->setPlaceholderText(QStringLiteral("Filter (Ctrl+Shift+B)"));
    connect(m_filter, &QLineEdit::textChanged, this, &ObjectBrowser::applyFilter);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setIndentation(14);            /* spec §4 */
    m_tree->setIconSize(QSize(16, 16));    /* spec §4: 16x16 image list */
    m_tree->setUniformRowHeights(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    if(const QColor c = ObjectBrowserColor::load(); c.isValid())
        m_tree->setStyleSheet(
            QStringLiteral("QTreeWidget::item:selected{background:%1}").arg(c.name()));
    connect(m_tree, &QTreeWidget::itemExpanded, this, &ObjectBrowser::onItemExpanded);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
        QTreeWidgetItem *item = m_tree->itemAt(pos);
        if(!item)
            return;
        const int kind = item->data(0, Qt::UserRole).toInt();
        QMenu menu(this);

        if(kind == KDatabase) {
            const QString db = item->text(0);
            menu.addAction(QStringLiteral("Create &Table…"), this,
                           [this, db] { emit createTableRequested(db); });
            QMenu *create = menu.addMenu(QStringLiteral("&Create Object"));
            for(const auto &kw : { QStringLiteral("VIEW"), QStringLiteral("PROCEDURE"),
                                   QStringLiteral("FUNCTION"), QStringLiteral("TRIGGER"),
                                   QStringLiteral("EVENT") }) {
                const QString t = kw;
                create->addAction(t.at(0) + t.mid(1).toLower() + QStringLiteral("…"),
                                  this, [this, db, t] { emit createObjectRequested(db, t); });
            }
            menu.addAction(QStringLiteral("&Copy Database…"), this,
                           [this, db] { emit copyDatabaseRequested(db); });
            menu.addAction(QStringLiteral("&Alter Database…"), this,
                           [this, db] { emit alterDatabaseRequested(db); });
            menu.addSeparator();
            menu.addAction(QStringLiteral("&Backup Database As SQL Dump…"), this,
                           [this, db] { emit dumpDatabaseRequested(db); });
            menu.addSeparator();
            menu.addAction(QStringLiteral("&Empty Database (truncate all tables)…"),
                           this, [this, db] { emit emptyDatabaseRequested(db); });
            menu.addAction(QStringLiteral("&Truncate Database (drop all objects)…"),
                           this, [this, db] { emit truncateDatabaseRequested(db); });
            menu.addAction(QStringLiteral("&Drop Database…"), this,
                           [this, db] { emit dropDatabaseRequested(db); });
        } else if(kind == KFolder
                  && item->text(0) == QStringLiteral("Tables")) {
            const QString db = item->data(0, Qt::UserRole + 1).toString();
            menu.addAction(QStringLiteral("Create &Table…"), this,
                           [this, db] { emit createTableRequested(db); });
        } else if(kind == KFolder && !folderObjType(item->text(0)).isEmpty()) {
            const QString db = item->data(0, Qt::UserRole + 1).toString();
            const QString t = folderObjType(item->text(0));
            menu.addAction(QStringLiteral("&Create %1…").arg(
                               t.at(0) + t.mid(1).toLower()),
                           this, [this, db, t] { emit createObjectRequested(db, t); });
        } else if(kind == KLeaf && item->parent()
                  && !folderObjType(item->parent()->text(0)).isEmpty()) {
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
                           [this, db, table] { emit tableActivated(db, table); });
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
                           [this, db, table] { copyCreateTable(db, table); });
            menu.addAction(QStringLiteral("Copy Column &Names"), this,
                           [this, item] { copyColumnNames(item); });
            menu.addAction(QStringLiteral("Re&fresh Node"), this,
                           [this, item] {
                item->takeChildren();
                onItemExpanded(item);
                item->setExpanded(true);
            });
        } else if(kind == KLeaf && item->parent()
                  && item->parent()->data(0, Qt::UserRole).toInt() == KFolder
                  && item->parent()->text(0) == QStringLiteral("Columns")) {
            const QString col = item->text(0).section(QStringLiteral("  :  "), 0, 0);
            menu.addAction(QStringLiteral("&Copy Column Name"), this, [col] {
                QApplication::clipboard()->setText(col);
            });
        }
        if(!menu.isEmpty())
            menu.exec(m_tree->viewport()->mapToGlobal(pos));
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                const int kind = item->data(0, Qt::UserRole).toInt();
                if(kind == KDatabase)
                    emit databaseActivated(item->data(0, Qt::UserRole + 1).toString());
                else if(kind == KTable)
                    emit tableActivated(item->data(0, Qt::UserRole + 1).toString(),
                                        item->text(0));
                else if(kind == KLeaf && item->parent()
                        && !folderObjType(item->parent()->text(0)).isEmpty())
                    emit alterObjectRequested(
                        item->parent()->data(0, Qt::UserRole + 1).toString(),
                        folderObjType(item->parent()->text(0)), item->text(0));
            });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_filterLabel);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree, 1);
}

void ObjectBrowser::setConnectionLabel(const QString &label)
{
    m_tree->clear();
    auto *root = makeItem(KConnection, label);
    root->setIcon(0, Icons::get(QStringLiteral("connect_16.ico")));
    m_tree->addTopLevelItem(root);
    m_tree->expandItem(root);
}

void ObjectBrowser::loadDatabases(IDbConnection *conn, const QString &currentDb)
{
    m_conn = conn;
    m_filterLabel->setText(QStringLiteral("Filter tables in %1")
                               .arg(currentDb.isEmpty() ? QStringLiteral("*")
                                                        : currentDb));
    QTreeWidgetItem *root = m_tree->topLevelItem(0);
    if(!root || !m_conn)
        return;

    root->takeChildren();
    for(const QString &dbName : m_conn->listDatabases()) {
        auto *db = makeItem(KDatabase, dbName, dbName);  /* carry db name */
        db->setIcon(0, Icons::get(QStringLiteral("database.ico")));
        root->addChild(db);
        if(currentDb == dbName) {
            db->setSelected(true);
            db->setExpanded(true);
            onItemExpanded(db);
            /* open the Tables folder straight away, like SQLyog */
            if(db->childCount() > 0) {
                QTreeWidgetItem *tablesFolder = db->child(0);
                tablesFolder->setExpanded(true);
                onItemExpanded(tablesFolder);
            }
        }
    }
    m_tree->expandItem(root);
}

QStringList ObjectBrowser::currentTableInfo() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if(!item || item->data(0, Qt::UserRole).toInt() != KTable)
        return {};
    return { item->data(0, Qt::UserRole + 1).toString(), item->text(0) };
}

void ObjectBrowser::onItemExpanded(QTreeWidgetItem *item)
{
    const int kind = item->data(0, Qt::UserRole).toInt();
    if(!m_conn || item->childCount() > 0)
        return;                       /* already populated, or not connected */

    const QString db = item->data(0, Qt::UserRole + 1).toString();

    if(kind == KDatabase) {
        /* SQLyog shows these six folders under every database, each with
         * its own icon (not a generic folder glyph) */
        static const QList<QPair<QString, QString>> kFolders = {
            { QStringLiteral("Tables"),       QStringLiteral("table.ico") },
            { QStringLiteral("Views"),        QStringLiteral("view.ico") },
            { QStringLiteral("Stored Procs"), QStringLiteral("process.ico") },
            { QStringLiteral("Functions"),    QStringLiteral("function.ico") },
            { QStringLiteral("Triggers"),     QStringLiteral("trigger.ico") },
            { QStringLiteral("Events"),       QStringLiteral("event.ico") },
        };
        for(const auto &[f, icon] : kFolders) {
            auto *folder = makeItem(KFolder, f, db);
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
            { QStringLiteral("Columns"), QStringLiteral("column.ico") },
            { QStringLiteral("Indexes"), QStringLiteral("index.ico") },
        };
        for(const auto &[sub, icon] : kSubFolders) {
            auto *f = makeItem(KFolder, sub, db);
            f->setData(0, Qt::UserRole + 2, item->text(0));
            f->setIcon(0, Icons::get(icon));
            item->addChild(f);
        }
        return;
    }

    if(kind != KFolder)
        return;

    const QString folder = item->text(0);

    /* table-scoped Columns / Indexes folder */
    if(const QString tbl = item->data(0, Qt::UserRole + 2).toString();
       !tbl.isEmpty()) {
        const auto add = [&](const QString &text, const QString &icon) {
            auto *l = makeItem(KLeaf, text);
            l->setIcon(0, Icons::get(icon));
            item->addChild(l);
        };
        if(folder == QStringLiteral("Columns")) {
            for(const QStringList &row : m_conn->listColumns(db, tbl).rows)
                add(QStringLiteral("%1  :  %2").arg(row.value(0), row.value(1)),
                    QStringLiteral("column.ico"));
        } else if(folder == QStringLiteral("Indexes")) {
            const DbResultSet rs = m_conn->listIndexes(db, tbl);
            QString curName;
            QStringList curCols;
            bool curUnique = false;
            const auto flush = [&] {
                if(curName.isEmpty()) return;
                add(QStringLiteral("%1  %2(%3)").arg(curName,
                        curUnique ? QStringLiteral("UNIQUE ") : QString(),
                        curCols.join(QStringLiteral(", "))),
                    QStringLiteral("altertable.ico"));
            };
            for(const QStringList &row : rs.rows) {
                const QString name = row.value(2);
                if(name != curName) { flush(); curName = name; curCols.clear();
                    curUnique = row.value(1) == QStringLiteral("0"); }
                if(!row.value(4).isEmpty()) curCols << row.value(4);
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
        for(const QString &t : m_conn->listTables(db, QStringLiteral("BASE TABLE"))) {
            auto *ti = makeItem(KTable, t, db);
            ti->setIcon(0, Icons::get(QStringLiteral("table.ico")));
            item->addChild(ti);
        }
    } else if(folder == QStringLiteral("Views")) {
        fillLeaves(item, m_conn->listTables(db, QStringLiteral("VIEW")),
                   QStringLiteral("alterview.ico"));
    } else if(folder == QStringLiteral("Stored Procs")) {
        QStringList names;
        for(const QStringList &row : m_conn->listRoutines(db).rows)
            if(row.value(1) == QStringLiteral("PROCEDURE"))
                names << row.value(0);
        fillLeaves(item, names, QStringLiteral("altersp.ico"));
    } else if(folder == QStringLiteral("Functions")) {
        QStringList names;
        for(const QStringList &row : m_conn->listRoutines(db).rows)
            if(row.value(1) == QStringLiteral("FUNCTION"))
                names << row.value(0);
        fillLeaves(item, names, QStringLiteral("alterfunction.ico"));
    } else if(folder == QStringLiteral("Triggers")) {
        fillLeaves(item, m_conn->listTriggers(db),
                   QStringLiteral("altertrigger.ico"));
    } else if(folder == QStringLiteral("Events")) {
        fillLeaves(item, m_conn->listEvents(db),
                   QStringLiteral("alterevent.ico"));
    }
}

void ObjectBrowser::collapseTree()
{
    m_tree->collapseAll();
    if(QTreeWidgetItem *root = m_tree->topLevelItem(0))
        root->setExpanded(true);   /* keep the connection node open */
}

void ObjectBrowser::copyCreateTable(const QString &db, const QString &table)
{
    if(!m_conn)
        return;
    QString error;
    const QString ddl = m_conn->showCreate(QStringLiteral("TABLE"), db, table, &error);
    if(ddl.isEmpty()) {
        if(!error.isEmpty())
            emit statusMessage(QStringLiteral("SHOW CREATE failed: %1").arg(error));
        return;
    }
    QApplication::clipboard()->setText(ddl + QLatin1Char(';'));
    emit statusMessage(QStringLiteral("CREATE statement for `%1` copied")
                           .arg(table));
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
        if(c->data(0, Qt::UserRole).toInt() == KFolder
           && c->text(0) == QStringLiteral("Columns")) {
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
        emit statusMessage(QStringLiteral("%1 column name(s) copied")
                               .arg(names.size()));
    }
}

void ObjectBrowser::applyFilter(const QString &text)
{
    /* filter the whole tree, matching SQLyog's "Filter tables" box */
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *item) {
        bool visible = text.isEmpty()
                       || item->text(0).contains(text, Qt::CaseInsensitive);
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
