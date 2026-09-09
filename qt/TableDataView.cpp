#include "TableDataView.h"
#include "wyString.h"

#include <QColor>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <cstring>

#include <mysql/mysql.h>

/* ---------------- editable model (staged) ---------------- */

class TableDataModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit TableDataModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setGrid(const QStringList &cols, const QVector<QStringList> &rows)
    {
        beginResetModel();
        m_cols = cols;
        m_rows = rows;
        m_orig = rows;                 /* baseline for dirty tracking */
        endResetModel();
        emit pendingChanged();
    }

    void removeRow_(int row)
    {
        beginRemoveRows({}, row, row);
        m_rows.removeAt(row);
        m_orig.removeAt(row);
        endRemoveRows();
        emit pendingChanged();
    }

    /* commit: the staged values become the new baseline */
    void commitAll()
    {
        m_orig = m_rows;
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
                         { Qt::BackgroundRole });
        emit pendingChanged();
    }

    void revertAll()
    {
        beginResetModel();
        m_rows = m_orig;
        endResetModel();
        emit pendingChanged();
    }

    bool dirty(int r, int c) const
    {
        return r < m_orig.size() && c < m_orig[r].size()
               && m_orig[r][c] != m_rows[r][c];
    }
    QList<int> dirtyRows() const
    {
        QList<int> out;
        for(int r = 0; r < m_rows.size(); ++r)
            for(int c = 0; c < m_cols.size(); ++c)
                if(dirty(r, c)) { out << r; break; }
        return out;
    }
    int pendingCells() const
    {
        int n = 0;
        for(int r = 0; r < m_rows.size(); ++r)
            for(int c = 0; c < m_cols.size(); ++c)
                if(dirty(r, c)) ++n;
        return n;
    }
    QString cur(int r, int c) const  { return m_rows.value(r).value(c); }
    QString orig(int r, int c) const { return m_orig.value(r).value(c); }
    QStringList columns() const { return m_cols; }

    void stage(int row, int col, const QString &value)
    {
        if(row < 0 || row >= m_rows.size() || col < 0 || col >= m_cols.size())
            return;
        m_rows[row][col] = value;
        emit dataChanged(index(row, col), index(row, col),
                         { Qt::DisplayRole, Qt::EditRole, Qt::BackgroundRole,
                           Qt::ForegroundRole });
        emit pendingChanged();
    }

    int rowCount(const QModelIndex & = {}) const override { return m_rows.size(); }
    int columnCount(const QModelIndex & = {}) const override { return m_cols.size(); }

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if(!idx.isValid())
            return {};
        const int r = idx.row(), c = idx.column();
        if(role == Qt::DisplayRole || role == Qt::EditRole)
            return m_rows[r][c];
        if(role == Qt::BackgroundRole && dirty(r, c))
            return QColor(0xFF, 0xF3, 0xC4);          /* staged edit — amber */
        if(role == Qt::ForegroundRole && m_rows[r][c] == QStringLiteral("NULL"))
            return QColor(Qt::gray);
        if(role == Qt::ToolTipRole && dirty(r, c))
            return QStringLiteral("was: %1").arg(m_orig[r][c]);
        return {};
    }

    QVariant headerData(int s, Qt::Orientation o, int role) const override
    {
        if(role != Qt::DisplayRole)
            return {};
        return o == Qt::Horizontal ? m_cols.value(s) : QVariant(s + 1);
    }

    Qt::ItemFlags flags(const QModelIndex &idx) const override
    {
        return QAbstractTableModel::flags(idx) | Qt::ItemIsEditable;
    }

    bool setData(const QModelIndex &idx, const QVariant &value, int role) override
    {
        if(role != Qt::EditRole || !idx.isValid())
            return false;
        stage(idx.row(), idx.column(), value.toString());
        return true;
    }

signals:
    void pendingChanged();

private:
    QStringList          m_cols;
    QVector<QStringList> m_rows;
    QVector<QStringList> m_orig;
};

/* ---------------- the view ---------------- */

TableDataView::TableDataView(QWidget *parent)
    : QWidget(parent)
{
    m_label = new QLabel(
        QStringLiteral("Double-click a table in the Object Browser to open it."),
        this);

    /* Apply / Revert bar — hidden until there are staged edits */
    m_applyBar = new QWidget(this);
    m_applyBtn = new QPushButton(QStringLiteral("&Apply"), m_applyBar);
    m_revertBtn = new QPushButton(QStringLiteral("&Revert"), m_applyBar);
    m_pendingLabel = new QLabel(m_applyBar);
    auto *barL = new QHBoxLayout(m_applyBar);
    barL->setContentsMargins(4, 2, 4, 2);
    barL->addWidget(m_applyBtn);
    barL->addWidget(m_revertBtn);
    barL->addWidget(m_pendingLabel);
    barL->addStretch(1);
    m_applyBar->hide();
    connect(m_applyBtn, &QPushButton::clicked, this,
            &TableDataView::applyPendingEdits);
    connect(m_revertBtn, &QPushButton::clicked, this,
            &TableDataView::revertPendingEdits);

    m_model = new TableDataModel(this);
    m_grid  = new QTableView(this);
    m_grid->setModel(m_model);
    m_grid->horizontalHeader()->setStretchLastSection(true);
    m_grid->setAlternatingRowColors(true);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_label);
    layout->addWidget(m_applyBar);
    layout->addWidget(m_grid, 1);

    connect(m_model, &TableDataModel::pendingChanged, this,
            &TableDataView::updateApplyBar);
    connect(m_grid, &QTableView::customContextMenuRequested, this,
            [this](const QPoint &pos) {
        if(!m_valid || !m_grid->indexAt(pos).isValid())
            return;
        const int pending = m_model->pendingCells();
        QMenu menu(this);
        QAction *setNull = menu.addAction(QStringLiteral("Set Cell Value to &NULL"),
                                          this, &TableDataView::setCellNull);
        setNull->setEnabled(m_grid->currentIndex().isValid());
        menu.addSeparator();
        QAction *apply = menu.addAction(QStringLiteral("A&pply Changes"), this,
                                        &TableDataView::applyPendingEdits);
        QAction *revert = menu.addAction(QStringLiteral("Re&vert Changes"), this,
                                         &TableDataView::revertPendingEdits);
        apply->setEnabled(pending > 0);
        revert->setEnabled(pending > 0);
        menu.addSeparator();
        menu.addAction(QStringLiteral("&Delete Row"), this,
                       &TableDataView::deleteSelectedRow);
        menu.addSeparator();
        menu.addAction(QStringLiteral("&Add Row"), this, &TableDataView::addRow);
        menu.addAction(QStringLiteral("Add Row (with &values…)"), this,
                       &TableDataView::insertRowWithValues);
        menu.addSeparator();
        menu.addAction(QStringLiteral("&Refresh"), this, &TableDataView::refresh);
        menu.exec(m_grid->viewport()->mapToGlobal(pos));
    });
}

void TableDataView::load(MYSQL *conn, const QString &db, const QString &table)
{
    m_conn = conn;
    m_db = db;
    m_table = table;
    reload();
}

void TableDataView::editCell(int row, int col, const QString &value)
{
    /* selftest entry: stage then commit immediately so the DB reflects it */
    m_model->stage(row, col, value);
    applyPendingEdits();
}

void TableDataView::stageCellOnly(int row, int col, const QString &value)
{
    m_model->stage(row, col, value);   /* selftest: leave it pending */
}

void TableDataView::clear()
{
    m_valid = false;
    m_model->setGrid({}, {});
    m_label->setText(
        QStringLiteral("Double-click a table in the Object Browser to open it."));
}

void TableDataView::updateApplyBar()
{
    const int cells = m_model->pendingCells();
    const int rows  = m_model->dirtyRows().size();
    m_applyBar->setVisible(cells > 0);
    m_applyBtn->setEnabled(cells > 0);
    m_revertBtn->setEnabled(cells > 0);
    m_pendingLabel->setText(
        cells > 0 ? QStringLiteral("%1 staged change(s) in %2 row(s)")
                        .arg(cells).arg(rows)
                  : QString());
}

void TableDataView::reload()
{
    /* columns + keys, upstream-style: SHOW COLUMNS + SHOW KEYS */
    QStringList pkeys;
    wyString keys;
    keys.Sprintf("SHOW KEYS FROM `%s`.`%s`", m_db.toUtf8().constData(),
                 m_table.toUtf8().constData());
    if(m_conn && mysql_query(m_conn, keys.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res)) {
                /* row: Table, Non_unique(1), Key_name(2), Seq(3), Column(4)… */
                if(row[1] && row[2] && row[4]
                   && strcmp(row[1], "0") == 0
                   && strcmp(row[2], "PRIMARY") == 0)
                    pkeys << QString::fromUtf8(row[4]);
            }
            mysql_free_result(res);
        }
    }

    m_columns.clear();
    wyString cols;
    cols.Sprintf("SHOW COLUMNS FROM `%s`.`%s`", m_db.toUtf8().constData(),
                 m_table.toUtf8().constData());
    m_colInfo.clear();
    if(m_conn && mysql_query(m_conn, cols.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res)) {
                if(!row[0])
                    continue;
                m_columns << QString::fromUtf8(row[0]);
                ColumnInfo ci;
                ci.name = m_columns.last();
                ci.nullable = row[3] && strcmp(row[3], "NO") != 0;
                ci.autoInc  = row[5] && strstr(row[5], "auto_increment") != nullptr;
                m_colInfo << ci;
            }
            mysql_free_result(res);
        }
    }
    if(m_columns.isEmpty()) {
        clear();
        emit statusMessage(QStringLiteral("cannot read columns of %1.%2")
                               .arg(m_db, m_table));
        return;
    }

    m_pkColumns.clear();
    for(int i = 0; i < m_columns.size(); ++i)
        if(pkeys.contains(m_columns[i]))
            m_pkColumns << i;
    m_hasPrimary = !m_pkColumns.isEmpty();   /* upstream: PK only, else all-cols */

    wyString data;
    data.Sprintf("SELECT * FROM `%s`.`%s` LIMIT 1000",
                 m_db.toUtf8().constData(), m_table.toUtf8().constData());
    QStringList header;
    QVector<QStringList> rows;
    if(m_conn && mysql_query(m_conn, data.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            for(unsigned int i = 0; i < mysql_num_fields(res); ++i) {
                MYSQL_FIELD *f = mysql_fetch_field(res);
                header << QString::fromUtf8(f->name);
            }
            while(MYSQL_ROW row = mysql_fetch_row(res)) {
                QStringList r;
                for(unsigned int i = 0; i < mysql_num_fields(res); ++i)
                    r << (row[i] ? QString::fromUtf8(row[i])
                                 : QStringLiteral("NULL"));
                rows << r;
            }
            mysql_free_result(res);
        }
    }
    m_model->setGrid(header, rows);
    m_valid = true;

    m_label->setText(QStringLiteral("%1.%2  —  %3 row(s)%4")
        .arg(m_db, m_table).arg(rows.size())
        .arg(m_hasPrimary ? QString()
                          : QStringLiteral("  ⚠ no primary key: row identity "
                                           "= entire row")));
    emit statusMessage(
        QStringLiteral("Opened %1.%2 (%3 rows)").arg(m_db, m_table)
            .arg(rows.size()));
}

QString TableDataView::quoteValue(const QString &v)
{
    if(v == QStringLiteral("NULL"))
        return QStringLiteral("NULL");
    QString s = v;
    s.replace('\\', QStringLiteral("\\\\"));
    s.replace('\'', QStringLiteral("\\'"));
    return '\'' + s + '\'';
}

QString TableDataView::whereFromOrigRow(int row) const
{
    QStringList conds;
    const auto addCol = [&](int col) {
        const QString v = m_model->orig(row, col);
        if(v == QStringLiteral("NULL"))
            conds << QStringLiteral("`%1` IS NULL").arg(m_columns[col]);
        else
            conds << QStringLiteral("`%1` = %2").arg(m_columns[col], quoteValue(v));
    };
    if(m_hasPrimary)
        for(int col : m_pkColumns)
            addCol(col);
    else
        for(int col = 0; col < m_columns.size(); ++col)
            addCol(col);
    return conds.join(QStringLiteral(" and "));
}

void TableDataView::applyPendingEdits()
{
    if(!m_valid || !m_conn)
        return;
    const QList<int> rows = m_model->dirtyRows();
    if(rows.isEmpty())
        return;

    mysql_query(m_conn, "START TRANSACTION");
    int cells = 0;
    for(int r : rows) {
        QStringList setParts;
        for(int c = 0; c < m_columns.size(); ++c) {
            if(!m_model->dirty(r, c))
                continue;
            const QString v = m_model->cur(r, c);
            setParts << QStringLiteral("`%1` = %2")
                            .arg(m_columns[c],
                                 v == QStringLiteral("NULL") ? QStringLiteral("NULL")
                                                             : quoteValue(v));
            ++cells;
        }
        const QString where = whereFromOrigRow(r);
        if(setParts.isEmpty() || where.isEmpty())
            continue;

        wyString q;
        q.Sprintf("UPDATE `%s`.`%s` SET %s WHERE %s LIMIT 1",
                  m_db.toUtf8().constData(), m_table.toUtf8().constData(),
                  setParts.join(QStringLiteral(", ")).toUtf8().constData(),
                  where.toUtf8().constData());
        if(mysql_query(m_conn, q.GetString()) != 0) {
            const QString err = QString::fromUtf8(mysql_error(m_conn));
            mysql_query(m_conn, "ROLLBACK");
            emit statusMessage(QStringLiteral("Apply failed (rolled back): %1")
                                   .arg(err));
            return;
        }
    }
    mysql_query(m_conn, "COMMIT");
    m_model->commitAll();
    emit statusMessage(QStringLiteral("Applied %1 change(s) across %2 row(s)")
                           .arg(cells).arg(rows.size()));
}

void TableDataView::revertPendingEdits()
{
    m_model->revertAll();
    emit statusMessage(QStringLiteral("Reverted staged changes"));
}

void TableDataView::deleteSelectedRow()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid() || !m_conn)
        return;
    if(m_model->pendingCells() > 0
       && QMessageBox::question(this, QStringLiteral("Delete Row"),
              QStringLiteral("There are staged edits. Delete this row now "
                             "anyway? (staged edits stay pending)"))
              != QMessageBox::Yes)
        return;
    const int row = idx.row();

    wyString q;
    q.Sprintf("DELETE FROM `%s`.`%s` WHERE %s LIMIT 1",
              m_db.toUtf8().constData(), m_table.toUtf8().constData(),
              whereFromOrigRow(row).toUtf8().constData());

    if(mysql_query(m_conn, q.GetString()) != 0) {
        emit statusMessage(QStringLiteral("DELETE failed: ")
                           + mysql_error(m_conn));
        return;
    }
    m_model->removeRow_(row);
    emit statusMessage(QStringLiteral("1 row deleted"));
}

void TableDataView::addRow()
{
    if(!m_valid || !m_conn)
        return;
    wyString q;
    q.Sprintf("INSERT INTO `%s`.`%s` () VALUES ()",
              m_db.toUtf8().constData(), m_table.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) != 0) {
        emit statusMessage(QStringLiteral("INSERT failed: ")
                           + mysql_error(m_conn));
        return;
    }
    reload();   /* picks up defaults/auto-increment from the server */
}

void TableDataView::setCellNull()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid())
        return;
    m_model->stage(idx.row(), idx.column(), QStringLiteral("NULL"));
}

void TableDataView::refresh()
{
    if(m_valid)
        reload();
}

/* empty fields are skipped; nullable empty fields become NULL */
void TableDataView::insertRowWithValues()
{
    if(!m_valid || !m_conn)
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Add Row — %1.%2").arg(m_db, m_table));
    auto *form = new QFormLayout(&dlg);
    struct Field { QLineEdit *edit; const ColumnInfo *col; };
    QVector<Field> fields;
    for(const ColumnInfo &c : std::as_const(m_colInfo)) {
        if(c.autoInc)
            continue;                       /* let the server number it */
        auto *edit = new QLineEdit(&dlg);
        edit->setPlaceholderText(c.nullable ? QStringLiteral("NULL") : QString());
        form->addRow(c.name + (c.nullable ? QString() : QStringLiteral(" *")), edit);
        fields.append({edit, &c});
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                         | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return;

    QStringList names, values;
    for(const Field &f : fields) {
        const QString v = f.edit->text();
        if(v.isEmpty())
            continue;                       /* not provided */
        names << '`' + f.col->name + '`';
        values << quoteValue(v);
    }
    if(names.isEmpty()) {
        addRow();                           /* all defaults */
        return;
    }

    wyString q;
    q.Sprintf("INSERT INTO `%s`.`%s` (%s) VALUES (%s)",
              m_db.toUtf8().constData(), m_table.toUtf8().constData(),
              names.join(", ").toUtf8().constData(),
              values.join(", ").toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) != 0) {
        emit statusMessage(QStringLiteral("INSERT failed: ")
                           + mysql_error(m_conn));
        return;
    }
    reload();
    emit statusMessage(QStringLiteral("1 row inserted into %1.%2").arg(m_db, m_table));
}

#include "TableDataView.moc"
