#include "CreateTableDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
const QStringList kTypes = {
    QStringLiteral("INT"),        QStringLiteral("BIGINT"),
    QStringLiteral("TINYINT"),    QStringLiteral("SMALLINT"),
    QStringLiteral("DECIMAL"),    QStringLiteral("FLOAT"),
    QStringLiteral("DOUBLE"),     QStringLiteral("VARCHAR"),
    QStringLiteral("CHAR"),       QStringLiteral("TEXT"),
    QStringLiteral("LONGTEXT"),   QStringLiteral("DATE"),
    QStringLiteral("DATETIME"),   QStringLiteral("TIMESTAMP"),
    QStringLiteral("TIME"),       QStringLiteral("JSON"),
    QStringLiteral("BLOB"),       QStringLiteral("ENUM"),
};

/* centred checkbox in a cell */
QWidget *checkCell(bool checked = false)
{
    auto *w = new QWidget;
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setAlignment(Qt::AlignCenter);
    auto *cb = new QCheckBox;
    cb->setChecked(checked);
    l->addWidget(cb);
    return w;
}

QCheckBox *cellBox(QWidget *cellWidget)
{
    return cellWidget ? cellWidget->findChild<QCheckBox *>() : nullptr;
}
} // namespace

CreateTableDialog::CreateTableDialog(QString database, QWidget *parent)
    : QDialog(parent), m_database(std::move(database))
{
    setWindowTitle(m_database.isEmpty()
                       ? QStringLiteral("Create Table")
                       : QStringLiteral("Create Table in `%1`").arg(m_database));
    resize(820, 420);

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QStringLiteral("table name"));

    m_engine = new QComboBox(this);
    m_engine->addItems({ QStringLiteral("InnoDB"), QStringLiteral("MyISAM"),
                         QStringLiteral("MEMORY"), QStringLiteral("ARCHIVE"),
                         QStringLiteral("CSV") });
    m_charset = new QComboBox(this);
    m_charset->addItems({ QStringLiteral("utf8mb4"), QStringLiteral("utf8"),
                          QStringLiteral("latin1"), QStringLiteral("ascii"),
                          QStringLiteral("binary") });

    auto *top = new QFormLayout;
    top->addRow(QStringLiteral("Table &Name"), m_name);
    auto *engineLbl = new QLabel(QStringLiteral("&Engine"), this);
    engineLbl->setBuddy(m_engine);
    auto *charsetLbl = new QLabel(QStringLiteral("&Charset"), this);
    charsetLbl->setBuddy(m_charset);
    auto *opts = new QHBoxLayout;
    opts->addWidget(engineLbl);
    opts->addWidget(m_engine);
    opts->addSpacing(12);
    opts->addWidget(charsetLbl);
    opts->addWidget(m_charset);
    opts->addStretch(1);
    top->addRow(QStringLiteral("Options"), opts);

    m_grid = new QTableWidget(0, ColCount, this);
    m_grid->setHorizontalHeaderLabels({
        QStringLiteral("Column Name"), QStringLiteral("Data Type"),
        QStringLiteral("Length"), QStringLiteral("Default"),
        QStringLiteral("PK?"), QStringLiteral("Not Null?"),
        QStringLiteral("Unsigned?"), QStringLiteral("Auto Incr?"),
        QStringLiteral("Comment") });
    m_grid->verticalHeader()->setDefaultSectionSize(24);
    m_grid->horizontalHeader()->setStretchLastSection(true);
    m_grid->horizontalHeader()->resizeSection(CName, 150);
    m_grid->horizontalHeader()->resizeSection(CType, 110);
    m_grid->horizontalHeader()->resizeSection(CLen, 60);
    m_grid->horizontalHeader()->resizeSection(CDefault, 90);
    for(int c : { CPk, CNotNull, CUnsigned, CAuto })
        m_grid->horizontalHeader()->resizeSection(c, 66);

    auto *addBtn = new QPushButton(QStringLiteral("&Add Column"), this);
    auto *delBtn = new QPushButton(QStringLiteral("&Remove Column"), this);
    connect(addBtn, &QPushButton::clicked, this, [this] { addColumnRow(); });
    connect(delBtn, &QPushButton::clicked, this,
            &CreateTableDialog::removeSelectedRow);
    auto *rowBtns = new QHBoxLayout;
    rowBtns->addWidget(addBtn);
    rowBtns->addWidget(delBtn);
    rowBtns->addStretch(1);

    m_preview = new QLineEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setStyleSheet(QStringLiteral("color:#3B7DBB;"));

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("&Create"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_grid, 1);
    layout->addLayout(rowBtns);
    layout->addWidget(m_preview);
    layout->addWidget(buttons);

    connect(m_name, &QLineEdit::textChanged, this,
            &CreateTableDialog::updatePreview);
    connect(m_grid, &QTableWidget::cellChanged, this,
            &CreateTableDialog::updatePreview);
    connect(m_engine, &QComboBox::currentTextChanged, this,
            &CreateTableDialog::updatePreview);
    connect(m_charset, &QComboBox::currentTextChanged, this,
            &CreateTableDialog::updatePreview);

    /* seed with an id INT PK AUTO_INCREMENT, like SQLyog's first row */
    addColumnRow(QStringLiteral("id"), QStringLiteral("INT"));
    if(auto *pk = cellBox(m_grid->cellWidget(0, CPk)))       pk->setChecked(true);
    if(auto *nn = cellBox(m_grid->cellWidget(0, CNotNull)))  nn->setChecked(true);
    if(auto *ai = cellBox(m_grid->cellWidget(0, CAuto)))     ai->setChecked(true);
    updatePreview();
}

void CreateTableDialog::addColumnRow(const QString &name, const QString &type)
{
    const int row = m_grid->rowCount();
    m_grid->insertRow(row);
    m_grid->setItem(row, CName, new QTableWidgetItem(name));

    auto *typeBox = new QComboBox;
    typeBox->setEditable(true);
    typeBox->addItems(kTypes);
    typeBox->setCurrentText(type.isEmpty() ? QStringLiteral("INT") : type);
    m_grid->setCellWidget(row, CType, typeBox);
    connect(typeBox, &QComboBox::currentTextChanged, this,
            &CreateTableDialog::updatePreview);

    m_grid->setItem(row, CLen, new QTableWidgetItem);
    m_grid->setItem(row, CDefault, new QTableWidgetItem);
    m_grid->setItem(row, CComment, new QTableWidgetItem);
    for(int c : { CPk, CNotNull, CUnsigned, CAuto }) {
        m_grid->setCellWidget(row, c, checkCell());
        if(auto *cb = cellBox(m_grid->cellWidget(row, c)))
            connect(cb, &QCheckBox::toggled, this,
                    &CreateTableDialog::updatePreview);
    }
    updatePreview();
}

void CreateTableDialog::removeSelectedRow()
{
    const int row = m_grid->currentRow();
    if(row >= 0 && m_grid->rowCount() > 1)
        m_grid->removeRow(row);
    updatePreview();
}

QString CreateTableDialog::buildSql() const
{
    const QString table = m_name->text().trimmed();
    if(table.isEmpty())
        return {};

    QStringList defs;
    QStringList pkCols;
    for(int r = 0; r < m_grid->rowCount(); ++r) {
        const QString col = m_grid->item(r, CName)
                                ? m_grid->item(r, CName)->text().trimmed()
                                : QString();
        if(col.isEmpty())
            continue;

        const auto *typeBox =
            qobject_cast<QComboBox *>(m_grid->cellWidget(r, CType));
        QString type = typeBox ? typeBox->currentText().trimmed().toUpper()
                               : QStringLiteral("INT");
        const QString len = m_grid->item(r, CLen)
                                ? m_grid->item(r, CLen)->text().trimmed()
                                : QString();
        if(!len.isEmpty())
            type += QStringLiteral("(%1)").arg(len);

        const bool pk  = cellBox(m_grid->cellWidget(r, CPk))
                         && cellBox(m_grid->cellWidget(r, CPk))->isChecked();
        const bool nn  = cellBox(m_grid->cellWidget(r, CNotNull))
                         && cellBox(m_grid->cellWidget(r, CNotNull))->isChecked();
        const bool un  = cellBox(m_grid->cellWidget(r, CUnsigned))
                         && cellBox(m_grid->cellWidget(r, CUnsigned))->isChecked();
        const bool ai  = cellBox(m_grid->cellWidget(r, CAuto))
                         && cellBox(m_grid->cellWidget(r, CAuto))->isChecked();
        const QString def = m_grid->item(r, CDefault)
                                ? m_grid->item(r, CDefault)->text().trimmed()
                                : QString();
        const QString comment = m_grid->item(r, CComment)
                                    ? m_grid->item(r, CComment)->text().trimmed()
                                    : QString();

        QString d = QStringLiteral("  `%1` %2").arg(col, type);
        if(un) d += QStringLiteral(" UNSIGNED");
        if(nn || pk) d += QStringLiteral(" NOT NULL");
        if(ai) d += QStringLiteral(" AUTO_INCREMENT");
        if(!def.isEmpty())
            d += QStringLiteral(" DEFAULT %1")
                     .arg(def.compare(QStringLiteral("NULL"), Qt::CaseInsensitive) == 0
                          || def.startsWith('\'') || def.contains('(')
                              ? def : QStringLiteral("'%1'").arg(def));
        if(!comment.isEmpty())
            d += QStringLiteral(" COMMENT '%1'")
                     .arg(QString(comment).replace('\'', QStringLiteral("''")));
        defs << d;
        if(pk)
            pkCols << QStringLiteral("`%1`").arg(col);
    }
    if(defs.isEmpty())
        return {};
    if(!pkCols.isEmpty())
        defs << QStringLiteral("  PRIMARY KEY (%1)").arg(pkCols.join(QStringLiteral(", ")));

    const QString qualified = m_database.isEmpty()
        ? QStringLiteral("`%1`").arg(table)
        : QStringLiteral("`%1`.`%2`").arg(m_database, table);
    return QStringLiteral("CREATE TABLE %1 (\n%2\n) ENGINE=%3 DEFAULT CHARSET=%4")
        .arg(qualified, defs.join(QStringLiteral(",\n")),
             m_engine->currentText(), m_charset->currentText());
}

void CreateTableDialog::updatePreview()
{
    const QString sql = buildSql();
    m_preview->setText(sql.isEmpty()
                           ? QStringLiteral("— fill in a table name and at least one column —")
                           : QString(sql).replace('\n', ' '));
}
