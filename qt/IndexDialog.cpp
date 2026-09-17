#include "IndexDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
QString qi(SqlDriverType driver, const QString &ident)
{
    if(driver == SqlDriverType::Postgres)
        return QLatin1Char('"') + QString(ident).replace(QLatin1Char('"'), QStringLiteral("\"\"")) +
               QLatin1Char('"');
    return QLatin1Char('`') + QString(ident).replace(QLatin1Char('`'), QStringLiteral("``")) +
           QLatin1Char('`');
}

/* DROP INDEX's index-name argument is schema-qualified with a dot on both
 * Postgres and SQLite — but a SQLite connection's `database` is routinely
 * empty (see IDbConnection::qualify()'s own doc comment on this exact
 * pattern), and an empty-then-dot prefix would be invalid syntax there */
QString qualifyIndexName(SqlDriverType driver, const QString &db, const QString &name)
{
    return db.isEmpty() ? qi(driver, name) : qi(driver, db) + QLatin1Char('.') + qi(driver, name);
}
} // namespace

IndexDialog::IndexDialog(QString database, QString table, const QList<IndexDef> &indexes,
                         QStringList tableColumns, QWidget *parent, SqlDriverType driver)
    : QDialog(parent), m_driver(driver), m_database(std::move(database)), m_table(std::move(table)),
      m_columns(std::move(tableColumns))
{
    setWindowTitle(QStringLiteral("Manage Indexes — `%1`").arg(m_table));
    resize(580, 460);

    m_grid = new QTableWidget(0, 3, this);
    m_grid->setObjectName(QStringLiteral("indexGrid")); /* test discoverability */
    m_grid->setMinimumHeight(150);
    m_grid->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Columns"), QStringLiteral("Unique")});
    m_grid->horizontalHeader()->setStretchLastSection(true);
    m_grid->horizontalHeader()->resizeSection(0, 160);
    m_grid->horizontalHeader()->resizeSection(1, 260);
    m_grid->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);

    for(const IndexDef &ix : indexes) {
        addRow(ix);
        if(!ix.primary)
            m_originalNames << ix.name;
    }

    auto *removeBtn = new QPushButton(QStringLiteral("&Remove Selected"), this);
    removeBtn->setObjectName(QStringLiteral("removeSelectedBtn")); /* test discoverability */
    connect(removeBtn, &QPushButton::clicked, this, &IndexDialog::removeSelected);

    /* --- add-new area --- */
    m_newName = new QLineEdit(this);
    m_newName->setObjectName(QStringLiteral("newIndexName")); /* test discoverability */
    m_newName->setPlaceholderText(QStringLiteral("new index name"));
    m_newUnique = new QCheckBox(QStringLiteral("Unique"), this);
    m_newCols = new QListWidget(this);
    m_newCols->setObjectName(QStringLiteral("newIndexCols")); /* test discoverability */
    m_newCols->setSelectionMode(QAbstractItemView::NoSelection);
    m_newCols->setMaximumHeight(110);
    for(const QString &c : m_columns) {
        auto *it = new QListWidgetItem(c, m_newCols);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(Qt::Unchecked);
    }
    auto *addBtn = new QPushButton(QStringLiteral("&Add Index"), this);
    addBtn->setObjectName(QStringLiteral("addIndexBtn")); /* test discoverability */
    connect(addBtn, &QPushButton::clicked, this, &IndexDialog::addPending);

    auto *addRowL = new QHBoxLayout;
    addRowL->addWidget(new QLabel(QStringLiteral("Add:"), this));
    addRowL->addWidget(m_newName, 1);
    addRowL->addWidget(m_newUnique);
    addRowL->addWidget(addBtn);

    m_preview = new QLineEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setStyleSheet(QStringLiteral("color:#3B7DBB;"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Apply"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(
        new QLabel(QStringLiteral("Indexes on `%1`.`%2`").arg(m_database, m_table), this));
    lay->addWidget(m_grid, 1);
    lay->addWidget(removeBtn, 0, Qt::AlignLeft);
    lay->addWidget(new QLabel(QStringLiteral("Columns for the new index:"), this));
    lay->addWidget(m_newCols);
    lay->addLayout(addRowL);
    lay->addWidget(m_preview);
    lay->addWidget(buttons);

    updatePreview();
}

void IndexDialog::addRow(const IndexDef &ix)
{
    const int r = m_grid->rowCount();
    m_grid->insertRow(r);
    auto *nameItem = new QTableWidgetItem(ix.primary ? QStringLiteral("PRIMARY") : ix.name);
    nameItem->setData(Qt::UserRole, ix.primary); /* true => not droppable here */
    m_grid->setItem(r, 0, nameItem);
    m_grid->setItem(r, 1, new QTableWidgetItem(ix.columns.join(QStringLiteral(", "))));
    m_grid->setItem(
        r, 2, new QTableWidgetItem(ix.primary || ix.unique ? QStringLiteral("yes") : QString()));
}

void IndexDialog::addPending()
{
    QStringList cols;
    for(int i = 0; i < m_newCols->count(); ++i)
        if(m_newCols->item(i)->checkState() == Qt::Checked)
            cols << m_newCols->item(i)->text();
    QString name = m_newName->text().trimmed();
    if(cols.isEmpty())
        return;
    if(name.isEmpty())
        name = QStringLiteral("idx_%1").arg(cols.join(QStringLiteral("_")));

    IndexDef ix;
    ix.name = name;
    ix.columns = cols;
    ix.unique = m_newUnique->isChecked();
    addRow(ix);
    m_grid->item(m_grid->rowCount() - 1, 0)
        ->setData(Qt::UserRole + 1, true); /* mark: newly added */

    m_newName->clear();
    m_newUnique->setChecked(false);
    for(int i = 0; i < m_newCols->count(); ++i)
        m_newCols->item(i)->setCheckState(Qt::Unchecked);
    updatePreview();
}

void IndexDialog::removeSelected()
{
    const int r = m_grid->currentRow();
    if(r < 0)
        return;
    if(m_grid->item(r, 0)->data(Qt::UserRole).toBool())
        return; /* PRIMARY — not here */
    m_grid->removeRow(r);
    updatePreview();
}

QString IndexDialog::buildSql() const
{
    /* neither Postgres nor SQLite has an ALTER TABLE ADD/DROP INDEX clause
     * at all — both need CREATE INDEX/DROP INDEX as their own top-level
     * statements (MySQL is the odd one out with an ALTER TABLE clause) */
    const bool standalone = m_driver != SqlDriverType::Mysql;
    QStringList current, adds;
    for(int r = 0; r < m_grid->rowCount(); ++r) {
        QTableWidgetItem *n = m_grid->item(r, 0);
        if(n->data(Qt::UserRole).toBool())
            continue; /* PRIMARY */
        const QString name = n->text();
        current << name;
        if(n->data(Qt::UserRole + 1).toBool()) { /* newly added row */
            const QString cols = m_grid->item(r, 1)->text();
            const bool uniq = m_grid->item(r, 2)->text() == QStringLiteral("yes");
            QStringList q;
            for(const QString &c : cols.split(QStringLiteral(", "), Qt::SkipEmptyParts))
                q << qi(m_driver, c.trimmed());
            if(standalone)
                adds << QStringLiteral("CREATE %1INDEX %2 ON %3 (%4)")
                            .arg(uniq ? QStringLiteral("UNIQUE ") : QString(), qi(m_driver, name),
                                 qi(m_driver, m_table), q.join(QStringLiteral(", ")));
            else
                adds << QStringLiteral("ADD %1INDEX `%2` (%3)")
                            .arg(uniq ? QStringLiteral("UNIQUE ") : QString(), name,
                                 q.join(QStringLiteral(", ")));
        }
    }

    if(standalone) {
        /* DROP INDEX takes a (schema-qualified) index name directly — no
         * "ON table" (unlike MySQL's older DROP INDEX … ON table form, or
         * its ALTER TABLE … DROP INDEX clause) */
        QStringList statements;
        for(const QString &orig : m_originalNames)
            if(!current.contains(orig))
                statements << QStringLiteral("DROP INDEX %1")
                                  .arg(qualifyIndexName(m_driver, m_database, orig));
        statements += adds;
        return statements.isEmpty() ? QString() : statements.join(QStringLiteral(";\n"));
    }

    QStringList clauses;
    for(const QString &orig : m_originalNames)
        if(!current.contains(orig))
            clauses << QStringLiteral("DROP INDEX `%1`").arg(orig);
    clauses += adds;

    if(clauses.isEmpty())
        return {};
    return QStringLiteral("ALTER TABLE `%1`.`%2`\n  %3")
        .arg(m_database, m_table, clauses.join(QStringLiteral(",\n  ")));
}

void IndexDialog::updatePreview()
{
    const QString sql = buildSql();
    m_preview->setText(sql.isEmpty() ? QStringLiteral("— no changes —")
                                     : QString(sql).replace('\n', ' '));
}
