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

QTreeWidgetItem *makeItem(int kind, const QString &name, const QString &extra = {})
{
    auto *item = new QTreeWidgetItem;
    item->setText(0, name);
    item->setData(0, Qt::UserRole, kind);
    item->setData(0, Qt::UserRole + 1, extra);
    if(kind == KDatabase || kind == KFolder)
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    return item;
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
        if(!item || item->data(0, Qt::UserRole).toInt() != KTable)
            return;
        const QString db = item->data(0, Qt::UserRole + 1).toString();
        const QString table = item->text(0);

        QMenu menu(this);
        menu.addAction(QStringLiteral("Open Table &Data"), this, [this, db, table] {
            emit tableActivated(db, table);
        });
        menu.addSeparator();
        menu.addAction(QStringLiteral("&Drop Table…"), this, [this, db, table] {
            emit dropTableRequested(db, table);
        });
        menu.addAction(QStringLiteral("&Truncate Table…"), this,
                       [this, db, table] { emit truncateTableRequested(db, table); });
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
                auto *db = makeItem(KDatabase, QString::fromUtf8(row[0]));
                db->setIcon(0, Icons::get(QStringLiteral("database.ico")));
                root->addChild(db);
                if(currentDb == row[0]) {
                    db->setSelected(true);
                    db->setExpanded(true);
                    onItemExpanded(db);
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

    if(kind == KFolder && item->childCount() == 0
       && item->text(0) == QStringLiteral("Tables")) {
        /* lazy-load the table list for this database */
        const QString db = item->data(0, Qt::UserRole + 1).toString();
        QString sql = QStringLiteral("SHOW TABLES FROM `%1`").arg(db);
        if(m_conn && mysql_query(m_conn, sql.toUtf8().constData()) == 0) {
            if(MYSQL_RES *res = mysql_store_result(m_conn)) {
                while(MYSQL_ROW row = mysql_fetch_row(res)) {
                    if(!row[0])
                        continue;
                    auto *t = makeItem(KTable, QString::fromUtf8(row[0]));
                    t->setIcon(0, Icons::get(QStringLiteral("table.ico")));
                    item->addChild(t);
                }
                mysql_free_result(res);
            }
        }
        return;
    }

    if(kind != KDatabase || item->childCount() > 0)
        return;

    /* SQLyog shows these six folders under every database */
    const QStringList folders = {
        QStringLiteral("Tables"), QStringLiteral("Views"),
        QStringLiteral("Stored Procs"), QStringLiteral("Functions"),
        QStringLiteral("Triggers"), QStringLiteral("Events")
    };
    for(const QString &f : folders) {
        auto *folder = makeItem(KFolder, f, item->data(0, Qt::UserRole + 1).toString());
        folder->setIcon(0, Icons::get(QStringLiteral("folder.ico")));
        item->addChild(folder);
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
