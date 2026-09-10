#include "ObjectBrowser.h"
#include "Icons.h"

#include <QMenu>
#include <QVBoxLayout>

#include <functional>

#include <mysql/mysql.h>

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

/* run a single-column query and append each value as a KLeaf child */
void fillLeaves(MYSQL *conn, QTreeWidgetItem *parent, const QString &sql,
                int col, const QString &icon)
{
    if(!conn || mysql_query(conn, sql.toUtf8().constData()) != 0)
        return;
    MYSQL_RES *res = mysql_store_result(conn);
    if(!res)
        return;
    while(MYSQL_ROW row = mysql_fetch_row(res)) {
        if(!row[col])
            continue;
        auto *leaf = makeItem(KLeaf, QString::fromUtf8(row[col]));
        leaf->setIcon(0, Icons::get(icon));
        parent->addChild(leaf);
    }
    mysql_free_result(res);
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
            menu.addSeparator();
            menu.addAction(QStringLiteral("Create &Table…"), this,
                           [this, db] { emit createTableRequested(db); });
            menu.addAction(QStringLiteral("&Drop Table…"), this,
                           [this, db, table] { emit dropTableRequested(db, table); });
            menu.addAction(QStringLiteral("&Truncate Table…"), this,
                           [this, db, table] { emit truncateTableRequested(db, table); });
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

void ObjectBrowser::loadDatabases(MYSQL *conn, const QString &currentDb)
{
    m_conn = conn;
    m_filterLabel->setText(QStringLiteral("Filter tables in %1")
                               .arg(currentDb.isEmpty() ? QStringLiteral("*")
                                                        : currentDb));
    QTreeWidgetItem *root = m_tree->topLevelItem(0);
    if(!root || !m_conn)
        return;

    root->takeChildren();
    if(mysql_query(m_conn, "SHOW DATABASES") == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res)) {
                if(!row[0])
                    continue;
                const QString dbName = QString::fromUtf8(row[0]);
                auto *db = makeItem(KDatabase, dbName, dbName);  /* carry db name */
                db->setIcon(0, Icons::get(QStringLiteral("database.ico")));
                root->addChild(db);
                if(currentDb == row[0]) {
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
            mysql_free_result(res);
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
        /* SQLyog shows these six folders under every database */
        for(const QString &f : { QStringLiteral("Tables"), QStringLiteral("Views"),
                                 QStringLiteral("Stored Procs"),
                                 QStringLiteral("Functions"),
                                 QStringLiteral("Triggers"), QStringLiteral("Events") }) {
            auto *folder = makeItem(KFolder, f, db);
            folder->setIcon(0, Icons::get(QStringLiteral("closed_folder.ico")));
            item->addChild(folder);
        }
        return;
    }

    if(kind == KTable) {
        /* columns of the table */
        const QString sql = QStringLiteral("SHOW COLUMNS FROM `%1`.`%2`")
                                .arg(db, item->text(0));
        if(mysql_query(m_conn, sql.toUtf8().constData()) == 0) {
            if(MYSQL_RES *res = mysql_store_result(m_conn)) {
                while(MYSQL_ROW row = mysql_fetch_row(res)) {
                    if(!row[0])
                        continue;
                    auto *c = makeItem(KLeaf,
                        QStringLiteral("%1  :  %2")
                            .arg(QString::fromUtf8(row[0]),
                                 QString::fromUtf8(row[1] ? row[1] : "")));
                    c->setIcon(0, Icons::get(QStringLiteral("column.ico")));
                    item->addChild(c);
                }
                mysql_free_result(res);
            }
        }
        return;
    }

    if(kind != KFolder)
        return;

    const QString bq = QString(db).replace('`', QStringLiteral("``"));
    const QString folder = item->text(0);
    if(folder == QStringLiteral("Tables")) {
        if(mysql_query(m_conn,
               QStringLiteral("SHOW FULL TABLES FROM `%1` WHERE Table_type='BASE TABLE'")
                   .arg(bq).toUtf8().constData()) == 0) {
            if(MYSQL_RES *res = mysql_store_result(m_conn)) {
                while(MYSQL_ROW row = mysql_fetch_row(res)) {
                    if(!row[0])
                        continue;
                    auto *t = makeItem(KTable, QString::fromUtf8(row[0]), db);
                    t->setIcon(0, Icons::get(QStringLiteral("table.ico")));
                    item->addChild(t);
                }
                mysql_free_result(res);
            }
        }
    } else if(folder == QStringLiteral("Views")) {
        fillLeaves(m_conn, item,
            QStringLiteral("SHOW FULL TABLES FROM `%1` WHERE Table_type='VIEW'").arg(bq),
            0, QStringLiteral("alterview.ico"));
    } else if(folder == QStringLiteral("Stored Procs")) {
        fillLeaves(m_conn, item,
            QStringLiteral("SELECT ROUTINE_NAME FROM information_schema.ROUTINES "
                           "WHERE ROUTINE_SCHEMA='%1' AND ROUTINE_TYPE='PROCEDURE'")
                .arg(bq),
            0, QStringLiteral("altersp.ico"));
    } else if(folder == QStringLiteral("Functions")) {
        fillLeaves(m_conn, item,
            QStringLiteral("SELECT ROUTINE_NAME FROM information_schema.ROUTINES "
                           "WHERE ROUTINE_SCHEMA='%1' AND ROUTINE_TYPE='FUNCTION'")
                .arg(bq),
            0, QStringLiteral("alterfunction.ico"));
    } else if(folder == QStringLiteral("Triggers")) {
        fillLeaves(m_conn, item,
            QStringLiteral("SHOW TRIGGERS FROM `%1`").arg(bq),
            0, QStringLiteral("altertrigger.ico"));
    } else if(folder == QStringLiteral("Events")) {
        fillLeaves(m_conn, item,
            QStringLiteral("SHOW EVENTS FROM `%1`").arg(bq),
            1, QStringLiteral("alterevent.ico"));   /* col 1 = Name */
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
