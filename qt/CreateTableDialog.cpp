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
const QStringList kMysqlTypes = {
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

/* no TINYINT/MEDIUMINT (use SMALLINT), no ENUM (needs its own CREATE TYPE
 * first), no unsigned-anything, no LONGTEXT (TEXT has no length cap here) —
 * left out rather than mapped to something misleading */
const QStringList kPostgresTypes = {
    QStringLiteral("INTEGER"),    QStringLiteral("BIGINT"),
    QStringLiteral("SMALLINT"),   QStringLiteral("NUMERIC"),
    QStringLiteral("REAL"),       QStringLiteral("DOUBLE PRECISION"),
    QStringLiteral("VARCHAR"),    QStringLiteral("CHAR"),
    QStringLiteral("TEXT"),       QStringLiteral("DATE"),
    QStringLiteral("TIMESTAMP"),  QStringLiteral("TIME"),
    QStringLiteral("BOOLEAN"),    QStringLiteral("JSONB"),
    QStringLiteral("BYTEA"),      QStringLiteral("UUID"),
};

QString qi(DriverType driver, const QString &ident)
{
    if(driver == DriverType::Postgres)
        return QLatin1Char('"') + QString(ident).replace(QLatin1Char('"'),
                                                          QStringLiteral("\"\""))
             + QLatin1Char('"');
    return QLatin1Char('`') + QString(ident).replace(QLatin1Char('`'),
                                                      QStringLiteral("``"))
         + QLatin1Char('`');
}

QString qualifyName(DriverType driver, const QString &db, const QString &name)
{
    return db.isEmpty() ? qi(driver, name)
                        : qi(driver, db) + QLatin1Char('.') + qi(driver, name);
}

QWidget *checkCell(bool checked = false, bool enabled = true)
{
    auto *w = new QWidget;
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setAlignment(Qt::AlignCenter);
    auto *cb = new QCheckBox;
    cb->setChecked(checked);
    cb->setEnabled(enabled);
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

CreateTableDialog::CreateTableDialog(QString database, QWidget *parent, DriverType driver)
    : QDialog(parent), m_mode(Mode::Create), m_driver(driver),
      m_database(std::move(database))
{
    setWindowTitle(m_database.isEmpty()
                       ? QStringLiteral("Create Table")
                       : QStringLiteral("Create Table in `%1`").arg(m_database));
    buildCommon();

    /* seed with an id INT PK AUTO_INCREMENT, like SQLyog's first row */
    addColumnRow(QStringLiteral("id"),
                 m_driver == DriverType::Postgres ? QStringLiteral("INTEGER")
                                                  : QStringLiteral("INT"));
    if(auto *pk = cellBox(m_grid->cellWidget(0, CPk)))       pk->setChecked(true);
    if(auto *nn = cellBox(m_grid->cellWidget(0, CNotNull)))  nn->setChecked(true);
    if(auto *ai = cellBox(m_grid->cellWidget(0, CAuto)))     ai->setChecked(true);
    updatePreview();
}

CreateTableDialog::CreateTableDialog(QString database, QString table,
                                     const QList<ColumnDef> &columns,
                                     QString engine, QString charset,
                                     QWidget *parent, DriverType driver)
    : QDialog(parent), m_mode(Mode::Alter), m_driver(driver),
      m_database(std::move(database)), m_table(std::move(table))
{
    setWindowTitle(QStringLiteral("Alter Table `%1`").arg(m_table));
    buildCommon();

    m_name->setText(m_table);
    m_name->setReadOnly(true);   /* rename via More Table Operations, not here */
    if(m_engine) {
        if(int i = m_engine->findText(engine, Qt::MatchFixedString); i >= 0)
            m_engine->setCurrentIndex(i);
    }
    if(m_charset) {
        if(int i = m_charset->findText(charset, Qt::MatchFixedString); i >= 0)
            m_charset->setCurrentIndex(i);
    }

    for(const ColumnDef &c : columns) {
        seedRow(c);
        m_originalCols << c.name;
        m_originalBody[c.name] = defBody(c);
        m_originalDefs[c.name] = c;
        if(c.pk)
            m_originalPk << c.name;
    }
    updatePreview();
}

void CreateTableDialog::buildCommon()
{
    resize(820, 420);
    const bool pg = m_driver == DriverType::Postgres;

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QStringLiteral("table name"));

    auto *top = new QFormLayout;
    top->addRow(QStringLiteral("Table &Name"), m_name);

    /* PostgreSQL has no storage engines or per-table charset at all — not
     * "not implemented yet" the way e.g. the SSL tab's other gaps are, so
     * hidden rather than shown-disabled */
    if(!pg) {
        m_engine = new QComboBox(this);
        m_engine->addItems({ QStringLiteral("InnoDB"), QStringLiteral("MyISAM"),
                             QStringLiteral("MEMORY"), QStringLiteral("ARCHIVE"),
                             QStringLiteral("CSV") });
        m_charset = new QComboBox(this);
        m_charset->addItems({ QStringLiteral("utf8mb4"), QStringLiteral("utf8"),
                              QStringLiteral("latin1"), QStringLiteral("ascii"),
                              QStringLiteral("binary") });
        connect(m_engine, &QComboBox::currentTextChanged, this,
                &CreateTableDialog::updatePreview);
        connect(m_charset, &QComboBox::currentTextChanged, this,
                &CreateTableDialog::updatePreview);

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
    }

    m_grid = new QTableWidget(0, ColCount, this);
    QStringList headers = {
        QStringLiteral("Column Name"), QStringLiteral("Data Type"),
        QStringLiteral("Length"), QStringLiteral("Default"),
        QStringLiteral("PK?"), QStringLiteral("Not Null?"),
        QStringLiteral("Unsigned?"), QStringLiteral("Auto Incr?"),
        QStringLiteral("Comment") };
    m_grid->setHorizontalHeaderLabels(headers);
    if(pg)
        m_grid->horizontalHeaderItem(CUnsigned)->setToolTip(
            QStringLiteral("PostgreSQL has no unsigned integer types"));
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
}

void CreateTableDialog::addColumnRow(const QString &name, const QString &type)
{
    const bool pg = m_driver == DriverType::Postgres;
    const int row = m_grid->rowCount();
    m_grid->insertRow(row);
    auto *nameItem = new QTableWidgetItem(name);
    nameItem->setData(Qt::UserRole, QString());   /* no original -> new column */
    m_grid->setItem(row, CName, nameItem);

    auto *typeBox = new QComboBox;
    typeBox->setEditable(true);
    typeBox->addItems(pg ? kPostgresTypes : kMysqlTypes);
    typeBox->setCurrentText(type.isEmpty()
        ? (pg ? QStringLiteral("INTEGER") : QStringLiteral("INT")) : type);
    m_grid->setCellWidget(row, CType, typeBox);
    connect(typeBox, &QComboBox::currentTextChanged, this,
            &CreateTableDialog::updatePreview);

    m_grid->setItem(row, CLen, new QTableWidgetItem);
    m_grid->setItem(row, CDefault, new QTableWidgetItem);
    m_grid->setItem(row, CComment, new QTableWidgetItem);
    for(int c : { CPk, CNotNull, CUnsigned, CAuto }) {
        m_grid->setCellWidget(row, c, checkCell(false, !(pg && c == CUnsigned)));
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

CreateTableDialog::ColumnDef CreateTableDialog::rowColumnDef(int row) const
{
    ColumnDef c;
    c.name = m_grid->item(row, CName) ? m_grid->item(row, CName)->text().trimmed()
                                      : QString();
    const auto *typeBox = qobject_cast<QComboBox *>(m_grid->cellWidget(row, CType));
    c.type = typeBox ? typeBox->currentText().trimmed().toUpper() : QStringLiteral("INT");
    c.length = m_grid->item(row, CLen) ? m_grid->item(row, CLen)->text().trimmed() : QString();
    c.def = m_grid->item(row, CDefault) ? m_grid->item(row, CDefault)->text().trimmed()
                                        : QString();
    c.comment = m_grid->item(row, CComment) ? m_grid->item(row, CComment)->text().trimmed()
                                            : QString();
    c.pk = cellBox(m_grid->cellWidget(row, CPk)) && cellBox(m_grid->cellWidget(row, CPk))->isChecked();
    c.notNull = cellBox(m_grid->cellWidget(row, CNotNull))
                && cellBox(m_grid->cellWidget(row, CNotNull))->isChecked();
    c.isUnsigned = cellBox(m_grid->cellWidget(row, CUnsigned))
                   && cellBox(m_grid->cellWidget(row, CUnsigned))->isChecked();
    c.autoInc = cellBox(m_grid->cellWidget(row, CAuto))
                && cellBox(m_grid->cellWidget(row, CAuto))->isChecked();
    return c;
}

/* "TYPE(len) UNSIGNED NOT NULL AUTO_INCREMENT DEFAULT x COMMENT 'y'" for a row
 * (MySQL/SQLite) or the PostgreSQL equivalent for CREATE TABLE's column list */
QString CreateTableDialog::rowBody(int row) const
{
    return defBody(rowColumnDef(row));
}

QString CreateTableDialog::defBody(const ColumnDef &c) const
{
    QString type = c.type.toUpper();
    if(!c.length.isEmpty())
        type += QStringLiteral("(%1)").arg(c.length);

    QString b = type;
    if(m_driver == DriverType::Postgres) {
        if(c.notNull || c.pk) b += QStringLiteral(" NOT NULL");
        if(!c.def.isEmpty())
            b += QStringLiteral(" DEFAULT %1").arg(formatDefault(c.def));
        if(c.autoInc)
            b += QStringLiteral(" GENERATED BY DEFAULT AS IDENTITY");
        /* comment has no place in a column definition on Postgres — it's
         * its own COMMENT ON COLUMN statement, added by buildCreateSql()/
         * buildAlterSqlPostgres() after the table exists */
        return b;
    }

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
    const bool pg = m_driver == DriverType::Postgres;
    const QString qualified = qualifyName(m_driver, m_database, table);

    QStringList defs, pkCols, comments;
    for(int r = 0; r < m_grid->rowCount(); ++r) {
        const ColumnDef c = rowColumnDef(r);
        if(c.name.isEmpty())
            continue;
        defs << QStringLiteral("  %1 %2").arg(qi(m_driver, c.name), defBody(c));
        if(c.pk)
            pkCols << qi(m_driver, c.name);
        if(pg && !c.comment.isEmpty())
            comments << QStringLiteral("COMMENT ON COLUMN %1.%2 IS '%3'")
                            .arg(qualified, qi(m_driver, c.name),
                                 QString(c.comment).replace('\'', QStringLiteral("''")));
    }
    if(defs.isEmpty())
        return {};
    if(!pkCols.isEmpty())
        defs << QStringLiteral("  PRIMARY KEY (%1)")
                    .arg(pkCols.join(QStringLiteral(", ")));

    QString sql = QStringLiteral("CREATE TABLE %1 (\n%2\n)")
        .arg(qualified, defs.join(QStringLiteral(",\n")));
    if(!pg)
        sql += QStringLiteral(" ENGINE=%1 DEFAULT CHARSET=%2")
                    .arg(m_engine->currentText(), m_charset->currentText());
    for(const QString &c : comments)
        sql += QStringLiteral(";\n") + c;
    return sql;
}

QString CreateTableDialog::buildAlterSql() const
{
    if(m_driver == DriverType::Postgres)
        return buildAlterSqlPostgres();

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

/* PostgreSQL's ALTER TABLE can combine ADD/DROP COLUMN and per-column
 * ALTER COLUMN {TYPE|SET/DROP NOT NULL|SET/DROP DEFAULT|ADD/DROP IDENTITY}
 * clauses with commas in one statement — but RENAME COLUMN must be its own
 * separate ALTER TABLE, and a comment change is its own top-level COMMENT
 * ON COLUMN statement, neither of which fits the clause list. Those extra
 * statements are appended after the main one, ';'-separated; PQexec runs
 * the whole string as one implicit transaction (see CreateTableDialog.h). */
QString CreateTableDialog::buildAlterSqlPostgres() const
{
    const QString qualified = qualifyName(m_driver, m_database, m_table);
    QStringList clauses, newPk, seenOrig, extraStatements;

    for(int r = 0; r < m_grid->rowCount(); ++r) {
        const ColumnDef c = rowColumnDef(r);
        QTableWidgetItem *nameItem = m_grid->item(r, CName);
        const QString orig = nameItem ? nameItem->data(Qt::UserRole).toString() : QString();
        if(c.name.isEmpty())
            continue;
        if(c.pk)
            newPk << c.name;

        if(orig.isEmpty()) {
            clauses << QStringLiteral("ADD COLUMN %1 %2").arg(qi(m_driver, c.name), defBody(c));
            if(!c.comment.isEmpty())
                extraStatements << QStringLiteral("COMMENT ON COLUMN %1.%2 IS '%3'")
                    .arg(qualified, qi(m_driver, c.name),
                         QString(c.comment).replace('\'', QStringLiteral("''")));
            continue;
        }

        seenOrig << orig;
        const ColumnDef &was = m_originalDefs.value(orig);
        const QString qOrig = qi(m_driver, orig);

        if(c.name != orig)
            extraStatements << QStringLiteral("ALTER TABLE %1 RENAME COLUMN %2 TO %3")
                .arg(qualified, qOrig, qi(m_driver, c.name));

        const QString wasType = was.length.isEmpty()
            ? was.type.toUpper() : QStringLiteral("%1(%2)").arg(was.type.toUpper(), was.length);
        const QString nowType = c.length.isEmpty()
            ? c.type.toUpper() : QStringLiteral("%1(%2)").arg(c.type.toUpper(), c.length);
        if(wasType != nowType)
            /* no USING clause: Postgres can fail this if the old and new
             * types aren't automatically castable (e.g. varchar -> integer
             * on a column that isn't all-numeric text) — confirmed against
             * a live server: "column ... cannot be cast automatically ...
             * HINT: You might need to specify USING ...". Guessing the
             * right cast expression well enough to add one automatically
             * isn't attempted; a failure here is Postgres saying the
             * requested change is ambiguous, not a bug to paper over. */
            clauses << QStringLiteral("ALTER COLUMN %1 TYPE %2").arg(qOrig, nowType);

        if(c.notNull != was.notNull)
            clauses << QStringLiteral("ALTER COLUMN %1 %2 NOT NULL")
                .arg(qOrig, c.notNull ? QStringLiteral("SET") : QStringLiteral("DROP"));

        if(c.def != was.def)
            clauses << (c.def.isEmpty()
                ? QStringLiteral("ALTER COLUMN %1 DROP DEFAULT").arg(qOrig)
                : QStringLiteral("ALTER COLUMN %1 SET DEFAULT %2")
                      .arg(qOrig, formatDefault(c.def)));

        if(c.autoInc != was.autoInc)
            clauses << (c.autoInc
                ? QStringLiteral("ALTER COLUMN %1 ADD GENERATED BY DEFAULT AS IDENTITY").arg(qOrig)
                : QStringLiteral("ALTER COLUMN %1 DROP IDENTITY IF EXISTS").arg(qOrig));

        if(c.comment != was.comment)
            extraStatements << QStringLiteral("COMMENT ON COLUMN %1.%2 IS %3")
                .arg(qualified, c.name != orig ? qi(m_driver, c.name) : qOrig,
                     c.comment.isEmpty() ? QStringLiteral("NULL")
                         : QStringLiteral("'%1'").arg(QString(c.comment)
                               .replace('\'', QStringLiteral("''"))));
    }

    for(const QString &oc : m_originalCols)
        if(!seenOrig.contains(oc))
            clauses << QStringLiteral("DROP COLUMN %1").arg(qi(m_driver, oc));

    QStringList a = newPk, b = m_originalPk;
    a.sort();
    b.sort();
    if(a != b) {
        if(!m_originalPk.isEmpty())
            /* Postgres names an unnamed "PRIMARY KEY (...)" table
             * constraint "<table>_pkey" by default — true for every PK this
             * dialog itself ever creates (buildCreateSql()/this function
             * never name one explicitly), but not guaranteed for a table
             * whose PK was created some other way with an explicit
             * constraint name. This dialog has no live connection to look
             * the real name up via pg_constraint (it only ever builds
             * text); a wrong guess fails loudly (DROP CONSTRAINT errors if
             * the name doesn't exist) rather than dropping something else. */
            clauses << QStringLiteral("DROP CONSTRAINT %1")
                .arg(qi(m_driver, m_table + QStringLiteral("_pkey")));
        if(!newPk.isEmpty()) {
            QStringList q;
            for(const QString &c : newPk)
                q << qi(m_driver, c);
            clauses << QStringLiteral("ADD PRIMARY KEY (%1)").arg(q.join(QStringLiteral(", ")));
        }
    }

    if(clauses.isEmpty() && extraStatements.isEmpty())
        return {};

    QStringList out;
    if(!clauses.isEmpty())
        out << QStringLiteral("ALTER TABLE %1\n  %2")
                   .arg(qualified, clauses.join(QStringLiteral(",\n  ")));
    out += extraStatements;
    return out.join(QStringLiteral(";\n"));
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
