#include "ForeignKeyDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
const QStringList kActions = {
    QStringLiteral("RESTRICT"),
    QStringLiteral("CASCADE"),
    QStringLiteral("SET NULL"),
    QStringLiteral("NO ACTION"),
};

QString qi(SqlDriverType driver, const QString &ident)
{
    if(driver == SqlDriverType::Postgres)
        return QLatin1Char('"') + QString(ident).replace(QLatin1Char('"'), QStringLiteral("\"\"")) +
               QLatin1Char('"');
    return QLatin1Char('`') + QString(ident).replace(QLatin1Char('`'), QStringLiteral("``")) +
           QLatin1Char('`');
}
} // namespace

ForeignKeyDialog::ForeignKeyDialog(QString database, QString table, const QList<FkDef> &fks,
                                   QStringList tableColumns, QStringList dbTables, QWidget *parent,
                                   SqlDriverType driver)
    : QDialog(parent), m_driver(driver), m_database(std::move(database)), m_table(std::move(table)),
      m_columns(std::move(tableColumns)), m_dbTables(std::move(dbTables))
{
    setWindowTitle(QStringLiteral("Foreign Keys — `%1`").arg(m_table));
    resize(640, 460);

    m_grid = new QTableWidget(0, 5, this);
    m_grid->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Column(s)"),
                                       QStringLiteral("References"), QStringLiteral("On Delete"),
                                       QStringLiteral("On Update")});
    m_grid->horizontalHeader()->setStretchLastSection(true);
    m_grid->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setMinimumHeight(150);
    for(const FkDef &fk : fks) {
        addRow(fk, false);
        m_originalNames << fk.name;
    }

    auto *removeBtn = new QPushButton(QStringLiteral("&Remove Selected"), this);
    connect(removeBtn, &QPushButton::clicked, this, &ForeignKeyDialog::removeSelected);

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QStringLiteral("constraint name (optional)"));
    m_localCol = new QComboBox(this);
    m_localCol->setObjectName(QStringLiteral("localCol")); /* test discoverability */
    m_localCol->setEditable(true);                         /* type "a, b" for a composite FK */
    m_localCol->addItems(m_columns);
    m_localCol->setCurrentText(QString());
    m_refTable = new QComboBox(this);
    m_refTable->setObjectName(QStringLiteral("refTable")); /* test discoverability */
    m_refTable->addItems(m_dbTables);
    m_refCol = new QLineEdit(this);
    m_refCol->setObjectName(QStringLiteral("refCol")); /* test discoverability */
    m_refCol->setPlaceholderText(QStringLiteral("referenced column(s), comma-separated to match"));
    m_onDelete = new QComboBox(this);
    m_onDelete->addItems(kActions);
    m_onUpdate = new QComboBox(this);
    m_onUpdate->addItems(kActions);
    auto *addBtn = new QPushButton(QStringLiteral("&Add Foreign Key"), this);
    addBtn->setObjectName(QStringLiteral("addFkBtn")); /* test discoverability */
    connect(addBtn, &QPushButton::clicked, this, &ForeignKeyDialog::addPending);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Name"), m_name);
    form->addRow(QStringLiteral("Local column(s)"), m_localCol);
    form->addRow(QStringLiteral("Referenced table"), m_refTable);
    form->addRow(QStringLiteral("Referenced column(s)"), m_refCol);
    form->addRow(QStringLiteral("On Delete"), m_onDelete);
    form->addRow(QStringLiteral("On Update"), m_onUpdate);

    m_preview = new QLineEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setStyleSheet(QStringLiteral("color:#3B7DBB;"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Apply"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(
        new QLabel(QStringLiteral("Foreign keys on `%1`.`%2`").arg(m_database, m_table), this));
    lay->addWidget(m_grid, 1);
    lay->addWidget(removeBtn, 0, Qt::AlignLeft);
    lay->addLayout(form);
    lay->addWidget(addBtn, 0, Qt::AlignLeft);
    lay->addWidget(m_preview);
    lay->addWidget(buttons);

    updatePreview();
}

void ForeignKeyDialog::addRow(const FkDef &fk, bool isNew)
{
    const int r = m_grid->rowCount();
    m_grid->insertRow(r);
    auto *n = new QTableWidgetItem(fk.name);
    n->setData(Qt::UserRole, isNew);
    n->setData(Qt::UserRole + 1, fk.columns.join(QStringLiteral(",")));
    n->setData(Qt::UserRole + 2, fk.refTable);
    n->setData(Qt::UserRole + 3, fk.refColumns.join(QStringLiteral(",")));
    n->setData(Qt::UserRole + 4, fk.onDelete);
    n->setData(Qt::UserRole + 5, fk.onUpdate);
    m_grid->setItem(r, 0, n);
    m_grid->setItem(r, 1, new QTableWidgetItem(fk.columns.join(QStringLiteral(", "))));
    m_grid->setItem(r, 2,
                    new QTableWidgetItem(QStringLiteral("%1 (%2)").arg(
                        fk.refTable, fk.refColumns.join(QStringLiteral(", ")))));
    m_grid->setItem(r, 3, new QTableWidgetItem(fk.onDelete));
    m_grid->setItem(r, 4, new QTableWidgetItem(fk.onUpdate));
}

void ForeignKeyDialog::addPending()
{
    const auto split = [](const QString &s) {
        QStringList out;
        for(const QString &p : s.split(',', Qt::SkipEmptyParts))
            out << p.trimmed();
        return out;
    };
    FkDef fk;
    fk.columns = split(m_localCol->currentText());
    fk.refColumns = split(m_refCol->text());
    fk.refTable = m_refTable->currentText();
    if(fk.columns.isEmpty() || fk.refTable.isEmpty() || fk.columns.size() != fk.refColumns.size()) {
        m_preview->setText(QStringLiteral("— local and referenced column counts must match —"));
        return;
    }
    fk.onDelete = m_onDelete->currentText();
    fk.onUpdate = m_onUpdate->currentText();
    fk.name = m_name->text().trimmed();
    if(fk.name.isEmpty())
        fk.name = QStringLiteral("fk_%1_%2").arg(m_table, fk.columns.join(QStringLiteral("_")));
    addRow(fk, true);

    m_name->clear();
    m_refCol->clear();
    m_localCol->setCurrentText(QString());
    updatePreview();
}

void ForeignKeyDialog::removeSelected()
{
    const int r = m_grid->currentRow();
    if(r >= 0)
        m_grid->removeRow(r);
    updatePreview();
}

QString ForeignKeyDialog::buildSql() const
{
    m_limitation.clear();
    if(m_driver == SqlDriverType::Sqlite) {
        /* neither adding nor dropping a foreign key constraint on an
         * existing table is possible via SQLite's ALTER TABLE at all —
         * both need a full create-new/copy-data/drop-old/rename rebuild,
         * which this dialog doesn't attempt (see the header comment) */
        QStringList changed;
        for(int r = 0; r < m_grid->rowCount(); ++r) {
            QTableWidgetItem *n = m_grid->item(r, 0);
            if(n->data(Qt::UserRole).toBool())
                changed << (n->text().isEmpty() ? QStringLiteral("(new foreign key)") : n->text());
        }
        for(const QString &orig : m_originalNames) {
            bool stillPresent = false;
            for(int r = 0; r < m_grid->rowCount(); ++r)
                if(m_grid->item(r, 0)->text() == orig) {
                    stillPresent = true;
                    break;
                }
            if(!stillPresent)
                changed << orig;
        }
        if(!changed.isEmpty())
            m_limitation = QStringLiteral("SQLite can't add or drop a foreign key on an existing "
                                          "table without rebuilding it (not attempted here): %1.")
                               .arg(changed.join(QStringLiteral(", ")));
        return {};
    }

    QStringList current, adds;
    for(int r = 0; r < m_grid->rowCount(); ++r) {
        QTableWidgetItem *n = m_grid->item(r, 0);
        current << n->text();
        if(!n->data(Qt::UserRole).toBool())
            continue; /* existing, untouched */
        const auto bt = [this](const QString &csv) {
            QStringList q;
            for(const QString &c : csv.split(',', Qt::SkipEmptyParts))
                q << qi(m_driver, c.trimmed());
            return q.join(QStringLiteral(", "));
        };
        adds << QStringLiteral("ADD CONSTRAINT %1 FOREIGN KEY (%2) REFERENCES %3 (%4) "
                               "ON DELETE %5 ON UPDATE %6")
                    .arg(qi(m_driver, n->text()), bt(n->data(Qt::UserRole + 1).toString()),
                         qi(m_driver, n->data(Qt::UserRole + 2).toString()),
                         bt(n->data(Qt::UserRole + 3).toString()),
                         n->data(Qt::UserRole + 4).toString(),
                         n->data(Qt::UserRole + 5).toString());
    }

    /* MySQL: DROP FOREIGN KEY name. Standard SQL/PostgreSQL: a foreign key
     * is just a constraint, dropped like any other — DROP CONSTRAINT name. */
    const QString dropKw = m_driver == SqlDriverType::Postgres ? QStringLiteral("DROP CONSTRAINT")
                                                               : QStringLiteral("DROP FOREIGN KEY");
    QStringList clauses;
    for(const QString &orig : m_originalNames)
        if(!current.contains(orig))
            clauses << QStringLiteral("%1 %2").arg(dropKw, qi(m_driver, orig));
    clauses += adds;

    if(clauses.isEmpty())
        return {};
    const QString qualified = qi(m_driver, m_database) + QLatin1Char('.') + qi(m_driver, m_table);
    return QStringLiteral("ALTER TABLE %1\n  %2")
        .arg(qualified, clauses.join(QStringLiteral(",\n  ")));
}

void ForeignKeyDialog::updatePreview()
{
    const QString sql = buildSql();
    m_preview->setText(sql.isEmpty() ? QStringLiteral("— no changes —")
                                     : QString(sql).replace('\n', ' '));
}
