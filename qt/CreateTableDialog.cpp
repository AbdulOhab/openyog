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

/* format a DEFAULT value: keep NULL / function calls / already-quoted as-is,
 * otherwise single-quote */
QString formatDefault(const QString &def)
{
    if(def.compare(QStringLiteral("NULL"), Qt::CaseInsensitive) == 0
       || def.startsWith('\'') || def.contains('('))
        return def;
    return QStringLiteral("'%1'").arg(def);
}
} // namespace

CreateTableDialog::CreateTableDialog(QString database, QWidget *parent)
    : QDialog(parent), m_mode(Mode::Create), m_database(std::move(database))
{
    setWindowTitle(m_database.isEmpty()
                       ? QStringLiteral("Create Table")
                       : QStringLiteral("Create Table in `%1`").arg(m_database));
    buildCommon();

    /* seed with an id INT PK AUTO_INCREMENT, like SQLyog's first row */
    addColumnRow(QStringLiteral("id"), QStringLiteral("INT"));
    if(auto *pk = cellBox(m_grid->cellWidget(0, CPk)))       pk->setChecked(true);
    if(auto *nn = cellBox(m_grid->cellWidget(0, CNotNull)))  nn->setChecked(true);
    if(auto *ai = cellBox(m_grid->cellWidget(0, CAuto)))     ai->setChecked(true);
    updatePreview();
}

CreateTableDialog::CreateTableDialog(QString database, QString table,
                                     const QList<ColumnDef> &columns,
                                     QString engine, QString charset,
                                     QWidget *parent)
    : QDialog(parent), m_mode(Mode::Alter), m_database(std::move(database)),
      m_table(std::move(table))
{
    setWindowTitle(QStringLiteral("Alter Table `%1`").arg(m_table));
    buildCommon();

    m_name->setText(m_table);
    m_name->setReadOnly(true);   /* rename via More Table Operations, not here */
    if(int i = m_engine->findText(engine, Qt::MatchFixedString); i >= 0)
        m_engine->setCurrentIndex(i);
    if(int i = m_charset->findText(charset, Qt::MatchFixedString); i >= 0)
        m_charset->setCurrentIndex(i);

    for(const ColumnDef &c : columns) {
        seedRow(c);
        m_originalCols << c.name;
        m_originalBody[c.name] = defBody(c);
        if(c.pk)
            m_originalPk << c.name;
    }
    updatePreview();
}

void CreateTableDialog::buildCommon()
{
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
    buttons->button(QDialogButtonBox::Ok)->setText(
        m_mode == Mode::Alter ? QStringLiteral("&Alter") : QStringLiteral("&Create"));
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
}

void CreateTableDialog::addColumnRow(const QString &name, const QString &type)
{
    const int row = m_grid->rowCount();
    m_grid->insertRow(row);
    auto *nameItem = new QTableWidgetItem(name);
    nameItem->setData(Qt::UserRole, QString());   /* no original -> new column */
    m_grid->setItem(row, CName, nameItem);

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

void CreateTableDialog::seedRow(const ColumnDef &c)
{
    addColumnRow(c.name, c.type);
    const int row = m_grid->rowCount() - 1;
    m_grid->item(row, CName)->setData(Qt::UserRole, c.name);   /* original */
    m_grid->item(row, CLen)->setText(c.length);
    m_grid->item(row, CDefault)->setText(c.def);
    m_grid->item(row, CComment)->setText(c.comment);
    const struct { Col col; bool on; } flags[] = {
        { CPk, c.pk }, { CNotNull, c.notNull },
        { CUnsigned, c.isUnsigned }, { CAuto, c.autoInc } };
    for(const auto &f : flags)
        if(auto *cb = cellBox(m_grid->cellWidget(row, f.col)))
            cb->setChecked(f.on);
}

void CreateTableDialog::removeSelectedRow()
{
    const int row = m_grid->currentRow();
    if(row >= 0 && m_grid->rowCount() > 1)
        m_grid->removeRow(row);
    updatePreview();
}

/* "TYPE(len) UNSIGNED NOT NULL AUTO_INCREMENT DEFAULT x COMMENT 'y'" for a row */
QString CreateTableDialog::rowBody(int row) const
{
    const auto *typeBox =
        qobject_cast<QComboBox *>(m_grid->cellWidget(row, CType));
    QString type = typeBox ? typeBox->currentText().trimmed().toUpper()
                           : QStringLiteral("INT");
    const QString len = m_grid->item(row, CLen)
                            ? m_grid->item(row, CLen)->text().trimmed() : QString();
    if(!len.isEmpty())
        type += QStringLiteral("(%1)").arg(len);

    const bool pk = cellBox(m_grid->cellWidget(row, CPk))
                    && cellBox(m_grid->cellWidget(row, CPk))->isChecked();
    const bool nn = cellBox(m_grid->cellWidget(row, CNotNull))
                    && cellBox(m_grid->cellWidget(row, CNotNull))->isChecked();
    const bool un = cellBox(m_grid->cellWidget(row, CUnsigned))
                    && cellBox(m_grid->cellWidget(row, CUnsigned))->isChecked();
    const bool ai = cellBox(m_grid->cellWidget(row, CAuto))
                    && cellBox(m_grid->cellWidget(row, CAuto))->isChecked();
    const QString def = m_grid->item(row, CDefault)
                            ? m_grid->item(row, CDefault)->text().trimmed() : QString();
    const QString comment = m_grid->item(row, CComment)
                                ? m_grid->item(row, CComment)->text().trimmed() : QString();

    QString b = type;
    if(un) b += QStringLiteral(" UNSIGNED");
    if(nn || pk) b += QStringLiteral(" NOT NULL");
    if(ai) b += QStringLiteral(" AUTO_INCREMENT");
    if(!def.isEmpty())
        b += QStringLiteral(" DEFAULT %1").arg(formatDefault(def));
    if(!comment.isEmpty())
        b += QStringLiteral(" COMMENT '%1'")
                 .arg(QString(comment).replace('\'', QStringLiteral("''")));
    return b;
}

QString CreateTableDialog::defBody(const ColumnDef &c)
{
    QString type = c.type.toUpper();
    if(!c.length.isEmpty())
        type += QStringLiteral("(%1)").arg(c.length);
    QString b = type;
    if(c.isUnsigned) b += QStringLiteral(" UNSIGNED");
    if(c.notNull || c.pk) b += QStringLiteral(" NOT NULL");
    if(c.autoInc) b += QStringLiteral(" AUTO_INCREMENT");
    if(!c.def.isEmpty())
        b += QStringLiteral(" DEFAULT %1").arg(formatDefault(c.def));
    if(!c.comment.isEmpty())
        b += QStringLiteral(" COMMENT '%1'")
                 .arg(QString(c.comment).replace('\'', QStringLiteral("''")));
    return b;
}

QString CreateTableDialog::buildSql() const
{
    return m_mode == Mode::Alter ? buildAlterSql() : buildCreateSql();
}

QString CreateTableDialog::buildCreateSql() const
{
    const QString table = m_name->text().trimmed();
    if(table.isEmpty())
        return {};

    QStringList defs, pkCols;
    for(int r = 0; r < m_grid->rowCount(); ++r) {
        const QString col = m_grid->item(r, CName)
                                ? m_grid->item(r, CName)->text().trimmed() : QString();
        if(col.isEmpty())
            continue;
        defs << QStringLiteral("  `%1` %2").arg(col, rowBody(r));
        if(cellBox(m_grid->cellWidget(r, CPk))
           && cellBox(m_grid->cellWidget(r, CPk))->isChecked())
            pkCols << QStringLiteral("`%1`").arg(col);
    }
    if(defs.isEmpty())
        return {};
    if(!pkCols.isEmpty())
        defs << QStringLiteral("  PRIMARY KEY (%1)")
                    .arg(pkCols.join(QStringLiteral(", ")));

    const QString qualified = m_database.isEmpty()
        ? QStringLiteral("`%1`").arg(table)
        : QStringLiteral("`%1`.`%2`").arg(m_database, table);
    return QStringLiteral("CREATE TABLE %1 (\n%2\n) ENGINE=%3 DEFAULT CHARSET=%4")
        .arg(qualified, defs.join(QStringLiteral(",\n")),
             m_engine->currentText(), m_charset->currentText());
}

QString CreateTableDialog::buildAlterSql() const
{
    QStringList clauses, newPk;
    QStringList seenOrig;

    for(int r = 0; r < m_grid->rowCount(); ++r) {
        QTableWidgetItem *nameItem = m_grid->item(r, CName);
        if(!nameItem)
            continue;
        const QString name = nameItem->text().trimmed();
        if(name.isEmpty())
            continue;
        const QString orig = nameItem->data(Qt::UserRole).toString();
        const QString body = rowBody(r);

        if(cellBox(m_grid->cellWidget(r, CPk))
           && cellBox(m_grid->cellWidget(r, CPk))->isChecked())
            newPk << name;

        if(orig.isEmpty()) {
            clauses << QStringLiteral("ADD COLUMN `%1` %2").arg(name, body);
        } else {
            seenOrig << orig;
            if(name == orig && body == m_originalBody.value(orig))
                continue;   /* unchanged column — no CHANGE clause */
            clauses << QStringLiteral("CHANGE COLUMN `%1` `%2` %3")
                           .arg(orig, name, body);
        }
    }

    for(const QString &oc : m_originalCols)
        if(!seenOrig.contains(oc))
            clauses << QStringLiteral("DROP COLUMN `%1`").arg(oc);

    QStringList a = newPk, b = m_originalPk;
    a.sort();
    b.sort();
    if(a != b) {
        if(!m_originalPk.isEmpty())
            clauses << QStringLiteral("DROP PRIMARY KEY");
        if(!newPk.isEmpty()) {
            QStringList q;
            for(const QString &c : newPk)
                q << QStringLiteral("`%1`").arg(c);
            clauses << QStringLiteral("ADD PRIMARY KEY (%1)")
                           .arg(q.join(QStringLiteral(", ")));
        }
    }

    if(clauses.isEmpty())
        return {};

    const QString qualified = m_database.isEmpty()
        ? QStringLiteral("`%1`").arg(m_table)
        : QStringLiteral("`%1`.`%2`").arg(m_database, m_table);
    return QStringLiteral("ALTER TABLE %1\n  %2")
        .arg(qualified, clauses.join(QStringLiteral(",\n  ")));
}

void CreateTableDialog::updatePreview()
{
    const QString sql = buildSql();
    if(!sql.isEmpty()) {
        m_preview->setText(QString(sql).replace('\n', ' '));
    } else if(m_mode == Mode::Alter) {
        m_preview->setText(QStringLiteral("— no changes —"));
    } else {
        m_preview->setText(
            QStringLiteral("— fill in a table name and at least one column —"));
    }
}
