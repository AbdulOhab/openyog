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
    QStringLiteral("INT"),      QStringLiteral("BIGINT"),    QStringLiteral("TINYINT"),
    QStringLiteral("SMALLINT"), QStringLiteral("DECIMAL"),   QStringLiteral("FLOAT"),
    QStringLiteral("DOUBLE"),   QStringLiteral("VARCHAR"),   QStringLiteral("CHAR"),
    QStringLiteral("TEXT"),     QStringLiteral("LONGTEXT"),  QStringLiteral("DATE"),
    QStringLiteral("DATETIME"), QStringLiteral("TIMESTAMP"), QStringLiteral("TIME"),
    QStringLiteral("JSON"),     QStringLiteral("BLOB"),      QStringLiteral("ENUM"),
};

/* no TINYINT/MEDIUMINT (use SMALLINT), no ENUM (needs its own CREATE TYPE
 * first), no unsigned-anything, no LONGTEXT (TEXT has no length cap here) —
 * left out rather than mapped to something misleading */
const QStringList kPostgresTypes = {
    QStringLiteral("INTEGER"), QStringLiteral("BIGINT"),    QStringLiteral("SMALLINT"),
    QStringLiteral("NUMERIC"), QStringLiteral("REAL"),      QStringLiteral("DOUBLE PRECISION"),
    QStringLiteral("VARCHAR"), QStringLiteral("CHAR"),      QStringLiteral("TEXT"),
    QStringLiteral("DATE"),    QStringLiteral("TIMESTAMP"), QStringLiteral("TIME"),
    QStringLiteral("BOOLEAN"), QStringLiteral("JSONB"),     QStringLiteral("BYTEA"),
    QStringLiteral("UUID"),
};

QString qi(SqlDriverType driver, const QString &ident)
{
    if(driver == SqlDriverType::Postgres)
        return QLatin1Char('"') + QString(ident).replace(QLatin1Char('"'), QStringLiteral("\"\"")) +
               QLatin1Char('"');
    return QLatin1Char('`') + QString(ident).replace(QLatin1Char('`'), QStringLiteral("``")) +
           QLatin1Char('`');
}

QString qualifyName(SqlDriverType driver, const QString &db, const QString &name)
{
    return db.isEmpty() ? qi(driver, name) : qi(driver, db) + QLatin1Char('.') + qi(driver, name);
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
    if(def.compare(QStringLiteral("NULL"), Qt::CaseInsensitive) == 0 || def.startsWith('\'') ||
       def.contains('('))
        return def;
    return QStringLiteral("'%1'").arg(def);
}
} // namespace

CreateTableDialog::CreateTableDialog(QString database, QWidget *parent, SqlDriverType driver)
    : QDialog(parent), m_mode(Mode::Create), m_driver(driver), m_database(std::move(database))
{
    setWindowTitle(m_database.isEmpty() ? QStringLiteral("Create Table")
                                        : QStringLiteral("Create Table in `%1`").arg(m_database));
    buildCommon();

    /* seed with an id INT PK AUTO_INCREMENT, like SQLyog's first row.
     * SQLite gets "INTEGER" specifically (not "INT") because its rowid-alias
     * auto-increment mechanism only kicks in for a column declared with
     * that exact type name — see buildCreateSql()'s PK/AUTOINCREMENT
     * handling below. */
    addColumnRow(QStringLiteral("id"), m_driver == SqlDriverType::Mysql
                                           ? QStringLiteral("INT")
                                           : QStringLiteral("INTEGER"));
    if(auto *pk = cellBox(m_grid->cellWidget(0, CPk)))
        pk->setChecked(true);
    if(auto *nn = cellBox(m_grid->cellWidget(0, CNotNull)))
        nn->setChecked(true);
    if(auto *ai = cellBox(m_grid->cellWidget(0, CAuto)))
        ai->setChecked(true);
    updatePreview();
}

CreateTableDialog::CreateTableDialog(QString database, QString table,
                                     const QList<ColumnDef> &columns, QString engine,
                                     QString charset, QWidget *parent, SqlDriverType driver)
    : QDialog(parent), m_mode(Mode::Alter), m_driver(driver), m_database(std::move(database)),
      m_table(std::move(table))
{
    setWindowTitle(QStringLiteral("Alter Table `%1`").arg(m_table));
    buildCommon();

    m_name->setText(m_table);
    m_name->setReadOnly(true); /* rename via More Table Operations, not here */
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
    const bool pg = m_driver == SqlDriverType::Postgres;
    const bool mysql = m_driver == SqlDriverType::Mysql;

    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("tableName")); /* test discoverability */
    m_name->setPlaceholderText(QStringLiteral("table name"));

    auto *top = new QFormLayout;
    top->addRow(QStringLiteral("Table &Name"), m_name);

    /* neither PostgreSQL nor SQLite has storage engines or per-table
     * charset at all — not "not implemented yet" the way e.g. the SSL
     * tab's other gaps are, so hidden rather than shown-disabled */
    if(mysql) {
        m_engine = new QComboBox(this);
        m_engine->addItems({QStringLiteral("InnoDB"), QStringLiteral("MyISAM"),
                            QStringLiteral("MEMORY"), QStringLiteral("ARCHIVE"),
                            QStringLiteral("CSV")});
        m_charset = new QComboBox(this);
        m_charset->addItems({QStringLiteral("utf8mb4"), QStringLiteral("utf8"),
                             QStringLiteral("latin1"), QStringLiteral("ascii"),
                             QStringLiteral("binary")});
        connect(m_engine, &QComboBox::currentTextChanged, this, &CreateTableDialog::updatePreview);
        connect(m_charset, &QComboBox::currentTextChanged, this, &CreateTableDialog::updatePreview);

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
    m_grid->setObjectName(QStringLiteral("columnGrid")); /* test discoverability */
    QStringList headers = {
        QStringLiteral("Column Name"), QStringLiteral("Data Type"),  QStringLiteral("Length"),
        QStringLiteral("Default"),     QStringLiteral("PK?"),        QStringLiteral("Not Null?"),
        QStringLiteral("Unsigned?"),   QStringLiteral("Auto Incr?"), QStringLiteral("Comment")};
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
    for(int c : {CPk, CNotNull, CUnsigned, CAuto})
        m_grid->horizontalHeader()->resizeSection(c, 66);

    auto *addBtn = new QPushButton(QStringLiteral("&Add Column"), this);
    addBtn->setObjectName(QStringLiteral("addColumnBtn")); /* test discoverability */
    auto *delBtn = new QPushButton(QStringLiteral("&Remove Column"), this);
    /* reorder: rows are otherwise append-only, yet column order is part of
     * the table's definition — CREATE mode bakes it into the emitted defs,
     * ALTER mode surfaces it as MySQL AFTER/FIRST (see buildAlterSql()) */
    auto *upBtn = new QPushButton(QStringLiteral("Move &Up"), this);
    upBtn->setObjectName(QStringLiteral("moveUpBtn")); /* test discoverability */
    auto *downBtn = new QPushButton(QStringLiteral("Move &Down"), this);
    downBtn->setObjectName(QStringLiteral("moveDownBtn")); /* test discoverability */
    connect(addBtn, &QPushButton::clicked, this, [this] { addColumnRow(); });
    connect(delBtn, &QPushButton::clicked, this, &CreateTableDialog::removeSelectedRow);
    connect(upBtn, &QPushButton::clicked, this, [this] { moveSelectedRow(-1); });
    connect(downBtn, &QPushButton::clicked, this, [this] { moveSelectedRow(1); });
    auto *rowBtns = new QHBoxLayout;
    rowBtns->addWidget(addBtn);
    rowBtns->addWidget(delBtn);
    rowBtns->addWidget(upBtn);
    rowBtns->addWidget(downBtn);
    rowBtns->addStretch(1);

    m_preview = new QLineEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setStyleSheet(QStringLiteral("color:#3B7DBB;"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)
        ->setText(m_mode == Mode::Alter ? QStringLiteral("&Alter") : QStringLiteral("&Create"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_grid, 1);
    layout->addLayout(rowBtns);
    layout->addWidget(m_preview);
    layout->addWidget(buttons);

    connect(m_name, &QLineEdit::textChanged, this, &CreateTableDialog::updatePreview);
    connect(m_grid, &QTableWidget::cellChanged, this, &CreateTableDialog::updatePreview);
}

void CreateTableDialog::addColumnRow(const QString &name, const QString &type)
{
    const bool pg = m_driver == SqlDriverType::Postgres;
    const int row = m_grid->rowCount();
    m_grid->insertRow(row);
    auto *nameItem = new QTableWidgetItem(name);
    nameItem->setData(Qt::UserRole, QString()); /* no original -> new column */
    m_grid->setItem(row, CName, nameItem);

    auto *typeBox = new QComboBox;
    typeBox->setEditable(true);
    typeBox->addItems(pg ? kPostgresTypes : kMysqlTypes);
    typeBox->setCurrentText(
        type.isEmpty() ? (pg ? QStringLiteral("INTEGER") : QStringLiteral("INT")) : type);
    m_grid->setCellWidget(row, CType, typeBox);
    connect(typeBox, &QComboBox::currentTextChanged, this, &CreateTableDialog::updatePreview);

    m_grid->setItem(row, CLen, new QTableWidgetItem);
    m_grid->setItem(row, CDefault, new QTableWidgetItem);
    m_grid->setItem(row, CComment, new QTableWidgetItem);
    for(int c : {CPk, CNotNull, CUnsigned, CAuto}) {
        m_grid->setCellWidget(row, c, checkCell(false, !(pg && c == CUnsigned)));
        if(auto *cb = cellBox(m_grid->cellWidget(row, c)))
            connect(cb, &QCheckBox::toggled, this, &CreateTableDialog::updatePreview);
    }
    updatePreview();
}

void CreateTableDialog::seedRow(const ColumnDef &c)
{
    addColumnRow(c.name, c.type);
    const int row = m_grid->rowCount() - 1;
    m_grid->item(row, CName)->setData(Qt::UserRole, c.name); /* original */
    m_grid->item(row, CLen)->setText(c.length);
    m_grid->item(row, CDefault)->setText(c.def);
    m_grid->item(row, CComment)->setText(c.comment);
    const struct
    {
        Col col;
        bool on;
    } flags[] = {{CPk, c.pk}, {CNotNull, c.notNull}, {CUnsigned, c.isUnsigned}, {CAuto, c.autoInc}};
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

void CreateTableDialog::moveSelectedRow(int delta)
{
    const int row = m_grid->currentRow();
    const int other = row + delta;
    if(row < 0 || other < 0 || other >= m_grid->rowCount())
        return;
    /* swap BY VALUE rather than shuffling QTableWidgetItem/cell-widget
     * pointers between rows — Qt owns each item/widget per cell, and
     * moving them is fragile in ways re-writing their contents isn't */
    const ColumnDef a = rowColumnDef(row), b = rowColumnDef(other);
    const QString origA = m_grid->item(row, CName)->data(Qt::UserRole).toString();
    const QString origB = m_grid->item(other, CName)->data(Qt::UserRole).toString();
    const auto writeRow = [this](int r, const ColumnDef &c, const QString &orig) {
        m_grid->item(r, CName)->setText(c.name);
        m_grid->item(r, CName)->setData(Qt::UserRole, orig);
        m_grid->item(r, CLen)->setText(c.length);
        m_grid->item(r, CDefault)->setText(c.def);
        m_grid->item(r, CComment)->setText(c.comment);
        if(auto *typeBox = qobject_cast<QComboBox *>(m_grid->cellWidget(r, CType)))
            typeBox->setCurrentText(c.type);
        const struct
        {
            Col col;
            bool on;
        } flags[] = {
            {CPk, c.pk}, {CNotNull, c.notNull}, {CUnsigned, c.isUnsigned}, {CAuto, c.autoInc}};
        for(const auto &f : flags)
            if(auto *cb = cellBox(m_grid->cellWidget(r, f.col)))
                cb->setChecked(f.on);
    };
    writeRow(row, b, origB);
    writeRow(other, a, origA);
    m_grid->setCurrentCell(other, m_grid->currentColumn());
    updatePreview();
}

QVector<CreateTableDialog::RowRef> CreateTableDialog::nonEmptyRows() const
{
    QVector<RowRef> rows;
    for(int r = 0; r < m_grid->rowCount(); ++r) {
        QTableWidgetItem *nameItem = m_grid->item(r, CName);
        if(!nameItem)
            continue;
        const QString name = nameItem->text().trimmed();
        if(!name.isEmpty())
            rows.append({r, name, nameItem->data(Qt::UserRole).toString()});
    }
    return rows;
}

QString CreateTableDialog::prevExistingOrig(const QVector<RowRef> &rows, int i)
{
    for(int j = i - 1; j >= 0; --j)
        if(!rows[j].orig.isEmpty())
            return rows[j].orig;
    return QString();
}

CreateTableDialog::ColumnDef CreateTableDialog::rowColumnDef(int row) const
{
    ColumnDef c;
    c.name = m_grid->item(row, CName) ? m_grid->item(row, CName)->text().trimmed() : QString();
    const auto *typeBox = qobject_cast<QComboBox *>(m_grid->cellWidget(row, CType));
    c.type = typeBox ? typeBox->currentText().trimmed().toUpper() : QStringLiteral("INT");
    c.length = m_grid->item(row, CLen) ? m_grid->item(row, CLen)->text().trimmed() : QString();
    c.def = m_grid->item(row, CDefault) ? m_grid->item(row, CDefault)->text().trimmed() : QString();
    c.comment =
        m_grid->item(row, CComment) ? m_grid->item(row, CComment)->text().trimmed() : QString();
    c.pk =
        cellBox(m_grid->cellWidget(row, CPk)) && cellBox(m_grid->cellWidget(row, CPk))->isChecked();
    c.notNull = cellBox(m_grid->cellWidget(row, CNotNull)) &&
                cellBox(m_grid->cellWidget(row, CNotNull))->isChecked();
    c.isUnsigned = cellBox(m_grid->cellWidget(row, CUnsigned)) &&
                   cellBox(m_grid->cellWidget(row, CUnsigned))->isChecked();
    c.autoInc = cellBox(m_grid->cellWidget(row, CAuto)) &&
                cellBox(m_grid->cellWidget(row, CAuto))->isChecked();
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
    if(m_driver == SqlDriverType::Postgres) {
        if(c.notNull || c.pk)
            b += QStringLiteral(" NOT NULL");
        if(!c.def.isEmpty())
            b += QStringLiteral(" DEFAULT %1").arg(formatDefault(c.def));
        if(c.autoInc)
            b += QStringLiteral(" GENERATED BY DEFAULT AS IDENTITY");
        /* comment has no place in a column definition on Postgres — it's
         * its own COMMENT ON COLUMN statement, added by buildCreateSql()/
         * buildAlterSqlPostgres() after the table exists */
        return b;
    }

    if(c.isUnsigned)
        b += QStringLiteral(" UNSIGNED");
    if(c.notNull || c.pk)
        b += QStringLiteral(" NOT NULL");
    /* SQLite has no AUTO_INCREMENT column modifier — auto-increment there is
     * a property of the PRIMARY KEY declaration itself (INTEGER PRIMARY KEY
     * [AUTOINCREMENT]), composed by buildCreateSql()/buildAlterSqlSqlite(),
     * not appended here */
    if(c.autoInc && m_driver == SqlDriverType::Mysql)
        b += QStringLiteral(" AUTO_INCREMENT");
    if(!c.def.isEmpty())
        b += QStringLiteral(" DEFAULT %1").arg(formatDefault(c.def));
    if(!c.comment.isEmpty() && m_driver == SqlDriverType::Mysql)
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
    const bool pg = m_driver == SqlDriverType::Postgres;
    const bool sqlite = m_driver == SqlDriverType::Sqlite;
    const QString qualified = qualifyName(m_driver, m_database, table);

    /* SQLite's auto-increment is a property of the PRIMARY KEY declaration
     * itself (rowid aliasing), not a column modifier — only possible for a
     * *single* PK column, and only reliable when its declared type is
     * exactly "INTEGER". When that shape matches and Auto Incr? is checked,
     * emit that column inline as "col INTEGER PRIMARY KEY [AUTOINCREMENT]"
     * instead of a separate trailing PRIMARY KEY (...) clause; a composite
     * PK (or a single PK column without Auto Incr? checked) falls back to
     * the normal trailing clause below, same as MySQL/Postgres. */
    QString sqlitePkAutoIncCol;
    if(sqlite) {
        int pkCount = 0;
        for(int r = 0; r < m_grid->rowCount(); ++r) {
            const ColumnDef c = rowColumnDef(r);
            if(c.pk) {
                ++pkCount;
                if(c.autoInc)
                    sqlitePkAutoIncCol = c.name;
            }
        }
        if(pkCount != 1)
            sqlitePkAutoIncCol.clear();
    }

    QStringList defs, pkCols, comments;
    for(int r = 0; r < m_grid->rowCount(); ++r) {
        const ColumnDef c = rowColumnDef(r);
        if(c.name.isEmpty())
            continue;
        if(!sqlitePkAutoIncCol.isEmpty() && c.name == sqlitePkAutoIncCol)
            defs << QStringLiteral("  %1 INTEGER PRIMARY KEY AUTOINCREMENT")
                        .arg(qi(m_driver, c.name));
        else
            defs << QStringLiteral("  %1 %2").arg(qi(m_driver, c.name), defBody(c));
        if(c.pk && c.name != sqlitePkAutoIncCol)
            pkCols << qi(m_driver, c.name);
        if(pg && !c.comment.isEmpty())
            comments << QStringLiteral("COMMENT ON COLUMN %1.%2 IS '%3'")
                            .arg(qualified, qi(m_driver, c.name),
                                 QString(c.comment).replace('\'', QStringLiteral("''")));
    }
    if(defs.isEmpty())
        return {};
    if(!pkCols.isEmpty())
        defs << QStringLiteral("  PRIMARY KEY (%1)").arg(pkCols.join(QStringLiteral(", ")));

    QString sql =
        QStringLiteral("CREATE TABLE %1 (\n%2\n)").arg(qualified, defs.join(QStringLiteral(",\n")));
    if(m_driver == SqlDriverType::Mysql)
        sql += QStringLiteral(" ENGINE=%1 DEFAULT CHARSET=%2")
                   .arg(m_engine->currentText(), m_charset->currentText());
    for(const QString &c : comments)
        sql += QStringLiteral(";\n") + c;
    return sql;
}

QString CreateTableDialog::buildAlterSql() const
{
    if(m_driver == SqlDriverType::Postgres)
        return buildAlterSqlPostgres();
    if(m_driver == SqlDriverType::Sqlite)
        return buildAlterSqlSqlite();

    QStringList clauses, newPk;
    QStringList seenOrig;

    /* Position rules (MySQL):
     * - an EXISTING column only "moved" when its predecessor among other
     *   EXISTING columns changed — a new column inserted above it doesn't
     *   displace it (the new column's own FIRST/AFTER does the inserting),
     *   so a repositioning CHANGE is judged against prevExistingOrig(),
     *   not the raw previous row.
     * - a NEW column appends at the table's end by default, so it only
     *   needs an explicit FIRST/AFTER when some row follows it in the grid.
     * AFTER names refer to post-ALTER names (current row names), and both
     * the ADD and the CHANGE clauses are emitted in grid order, so a new
     * column's ADD always precedes any CHANGE that positions relative to
     * it within the one ALTER TABLE statement. */
    const QVector<RowRef> rows = nonEmptyRows();
    for(int i = 0; i < rows.count(); ++i) {
        const int r = rows[i].row;
        const QString &name = rows[i].name;
        const QString &orig = rows[i].orig;
        const QString body = rowBody(r);

        if(cellBox(m_grid->cellWidget(r, CPk)) && cellBox(m_grid->cellWidget(r, CPk))->isChecked())
            newPk << name;

        /* FIRST, or AFTER the previous row's current name */
        const QString place = [this, &rows, i]() {
            if(i == 0)
                return QStringLiteral(" FIRST");
            return QStringLiteral(" AFTER `%1`").arg(rows[i - 1].name);
        }();

        if(orig.isEmpty()) {
            if(i < rows.count() - 1) /* not the last row => MySQL's
                                      * append-at-end default is wrong */
                clauses << QStringLiteral("ADD COLUMN `%1` %2%3").arg(name, body, place);
            else
                clauses << QStringLiteral("ADD COLUMN `%1` %2").arg(name, body);
        } else {
            seenOrig << orig;
            const bool moved =
                prevExistingOrig(rows, i) != m_originalCols.value(m_originalCols.indexOf(orig) - 1);
            if(name == orig && body == m_originalBody.value(orig) && !moved)
                continue; /* unchanged column — no CHANGE clause */
            clauses << QStringLiteral("CHANGE COLUMN `%1` `%2` %3%4")
                           .arg(orig, name, body, moved ? place : QString());
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
            clauses << QStringLiteral("ADD PRIMARY KEY (%1)").arg(q.join(QStringLiteral(", ")));
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

/* SQLite's ALTER TABLE supports exactly one clause per statement — ADD
 * COLUMN, DROP COLUMN, RENAME COLUMN or RENAME TO — and none of them can
 * change an existing column's type/NOT NULL/DEFAULT, add or drop a PRIMARY
 * KEY, or add/drop AUTOINCREMENT; those all need the classic
 * create-new-table/copy-data/drop-old/rename-new rebuild, which this
 * dialog doesn't attempt. This emits the subset SQLite genuinely supports
 * directly (each as its own statement — SQLite doesn't allow comma-joining
 * multiple ALTER TABLE clauses the way MySQL/Postgres do); anything else
 * populates alterLimitation() so the caller can tell the user what wasn't
 * done, instead of either silently dropping it or sending SQL that would
 * fail outright. */
QString CreateTableDialog::buildAlterSqlSqlite() const
{
    m_alterLimitation.clear();
    const QString qualified = qualifyName(m_driver, m_database, m_table);
    QStringList stmts, newPk, seenOrig, limited;

    for(int r = 0; r < m_grid->rowCount(); ++r) {
        QTableWidgetItem *nameItem = m_grid->item(r, CName);
        if(!nameItem)
            continue;
        const QString name = nameItem->text().trimmed();
        if(name.isEmpty())
            continue;
        const QString orig = nameItem->data(Qt::UserRole).toString();
        const ColumnDef c = rowColumnDef(r);
        if(c.pk)
            newPk << name;

        if(orig.isEmpty()) {
            /* ADD COLUMN can't add a PRIMARY KEY/UNIQUE column, and a
             * NOT NULL column needs a non-NULL DEFAULT — there's no
             * existing row value to backfill with otherwise */
            if(c.pk || c.autoInc)
                limited << QStringLiteral("%1 (new primary key/auto-increment column)").arg(name);
            else if(c.notNull && c.def.isEmpty())
                limited << QStringLiteral("%1 (NOT NULL column needs a DEFAULT to add)").arg(name);
            else
                stmts << QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                             .arg(qualified, qi(m_driver, name), rowBody(r));
            continue;
        }

        seenOrig << orig;
        const ColumnDef &was = m_originalDefs.value(orig);
        if(name != orig)
            stmts << QStringLiteral("ALTER TABLE %1 RENAME COLUMN %2 TO %3")
                         .arg(qualified, qi(m_driver, orig), qi(m_driver, name));
        if(rowBody(r) != m_originalBody.value(orig) || c.autoInc != was.autoInc)
            limited << QStringLiteral("%1 (column type/constraint change)").arg(name);
    }

    for(const QString &oc : m_originalCols)
        if(!seenOrig.contains(oc))
            stmts
                << QStringLiteral("ALTER TABLE %1 DROP COLUMN %2").arg(qualified, qi(m_driver, oc));

    /* SQLite's ALTER TABLE can't reposition a column either — no FIRST/
     * AFTER clause exists and ADD COLUMN only ever appends — same flag-
     * don't-attempt treatment as the type/PK changes above */
    {
        const QVector<RowRef> rows = nonEmptyRows();
        for(int i = 0; i < rows.count(); ++i) {
            const bool moved =
                rows[i].orig.isEmpty()
                    ? i < rows.count() - 1 /* new column not appended last */
                    : prevExistingOrig(rows, i) !=
                          m_originalCols.value(m_originalCols.indexOf(rows[i].orig) - 1);
            if(moved)
                limited << QStringLiteral("%1 (column reorder)").arg(rows[i].name);
        }
    }

    QStringList a = newPk, b = m_originalPk;
    a.sort();
    b.sort();
    if(a != b)
        limited << QStringLiteral("primary key change");

    if(!limited.isEmpty())
        m_alterLimitation = QStringLiteral("SQLite can't change these without rebuilding the table "
                                           "(not attempted here): %1.")
                                .arg(limited.join(QStringLiteral("; ")));

    return stmts.join(QStringLiteral(";\n"));
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
    m_alterLimitation.clear();
    const QString qualified = qualifyName(m_driver, m_database, m_table);
    QStringList clauses, newPk, seenOrig, extraStatements;

    /* PostgreSQL's ALTER TABLE has no way to reposition a column at all —
     * physical column order is fixed at creation; changing it needs a
     * drop-and-recreate (losing data) or full table rebuild. Same story
     * for placing a NEW column anywhere but the end (ADD COLUMN always
     * appends). Report via alterLimitation() and apply everything else,
     * rather than emitting SQL that silently lands columns elsewhere than
     * the grid shows. */
    {
        const QVector<RowRef> rows = nonEmptyRows();
        QStringList reordered;
        for(int i = 0; i < rows.count(); ++i) {
            if(rows[i].orig.isEmpty()) {
                if(i < rows.count() - 1) /* not appended last */
                    reordered << rows[i].name;
            } else if(prevExistingOrig(rows, i) !=
                      m_originalCols.value(m_originalCols.indexOf(rows[i].orig) - 1)) {
                reordered << rows[i].name;
            }
        }
        if(!reordered.isEmpty())
            m_alterLimitation =
                QStringLiteral("PostgreSQL can't reposition columns with ALTER TABLE (needs a "
                               "table rebuild, not attempted here); order of: %1 was left "
                               "unchanged.")
                    .arg(reordered.join(QStringLiteral(", ")));
    }

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
                                    ? was.type.toUpper()
                                    : QStringLiteral("%1(%2)").arg(was.type.toUpper(), was.length);
        const QString nowType = c.length.isEmpty()
                                    ? c.type.toUpper()
                                    : QStringLiteral("%1(%2)").arg(c.type.toUpper(), c.length);
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
            clauses << (c.def.isEmpty() ? QStringLiteral("ALTER COLUMN %1 DROP DEFAULT").arg(qOrig)
                                        : QStringLiteral("ALTER COLUMN %1 SET DEFAULT %2")
                                              .arg(qOrig, formatDefault(c.def)));

        if(c.autoInc != was.autoInc)
            clauses << (c.autoInc
                            ? QStringLiteral("ALTER COLUMN %1 ADD GENERATED BY DEFAULT AS IDENTITY")
                                  .arg(qOrig)
                            : QStringLiteral("ALTER COLUMN %1 DROP IDENTITY IF EXISTS").arg(qOrig));

        if(c.comment != was.comment)
            extraStatements << QStringLiteral("COMMENT ON COLUMN %1.%2 IS %3")
                                   .arg(qualified, c.name != orig ? qi(m_driver, c.name) : qOrig,
                                        c.comment.isEmpty()
                                            ? QStringLiteral("NULL")
                                            : QStringLiteral("'%1'").arg(QString(c.comment).replace(
                                                  '\'', QStringLiteral("''"))));
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
        m_preview->setText(QStringLiteral("— fill in a table name and at least one column —"));
    }
}
