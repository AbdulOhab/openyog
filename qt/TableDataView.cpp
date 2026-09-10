#include "TableDataView.h"
#include "wyString.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QFontDatabase>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstring>

#include <mysql/mysql.h>

/* ---------------- editable model (staged edits + inserts + deletes) ------- */

class TableDataModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum RowState { Normal, Inserted, Deleted };

    explicit TableDataModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setGrid(const QStringList &cols, const QVector<QStringList> &rows)
    {
        beginResetModel();
        m_cols = cols;
        m_rows = rows;
        m_orig = rows;                 /* baseline for dirty tracking */
        m_state = QVector<RowState>(rows.size(), Normal);
        endResetModel();
        emit pendingChanged();
    }

    /* append a blank pending-insert row; returns its index */
    int stageNewRow()
    {
        const int r = m_rows.size();
        const QStringList blank(m_cols.size());
        beginInsertRows({}, r, r);
        m_rows << blank;
        m_orig << blank;
        m_state << Inserted;
        endInsertRows();
        emit pendingChanged();
        return r;
    }

    /* Normal<->Deleted toggle; an Inserted row is just removed */
    void toggleDeleted(int row)
    {
        if(row < 0 || row >= m_rows.size())
            return;
        if(m_state[row] == Inserted) {
            beginRemoveRows({}, row, row);
            m_rows.removeAt(row);
            m_orig.removeAt(row);
            m_state.removeAt(row);
            endRemoveRows();
        } else {
            m_state[row] = (m_state[row] == Deleted) ? Normal : Deleted;
            emit dataChanged(index(row, 0), index(row, m_cols.size() - 1));
        }
        emit pendingChanged();
    }

    void revertAll()
    {
        beginResetModel();
        for(int r = m_rows.size() - 1; r >= 0; --r)
            if(m_state[r] == Inserted) {
                m_rows.removeAt(r);
                m_orig.removeAt(r);
                m_state.removeAt(r);
            }
        m_rows = m_orig;
        for(auto &s : m_state)
            s = Normal;
        endResetModel();
        emit pendingChanged();
    }

    bool dirty(int r, int c) const
    {
        return r < m_orig.size() && c < m_orig[r].size()
               && m_state.value(r) == Normal && m_orig[r][c] != m_rows[r][c];
    }
    RowState rowState(int r) const { return m_state.value(r, Normal); }

    QList<int> rowsWithState(RowState st) const
    {
        QList<int> out;
        for(int r = 0; r < m_state.size(); ++r)
            if(m_state[r] == st)
                out << r;
        return out;
    }
    QList<int> dirtyRows() const                 /* Normal rows with edits only */
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
    /* total staged operations (edits by row + inserts + deletes) */
    int pendingOps() const
    {
        return dirtyRows().size() + rowsWithState(Inserted).size()
             + rowsWithState(Deleted).size();
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
        if(role == Qt::BackgroundRole) {
            if(m_state[r] == Inserted) return QColor(0xE6, 0xF4, 0xEA); /* green */
            if(m_state[r] == Deleted)  return QColor(0xFD, 0xE7, 0xE9); /* red   */
            if(dirty(r, c))            return QColor(0xFF, 0xF3, 0xC4); /* amber */
        }
        if(role == Qt::FontRole && m_state[r] == Deleted) {
            QFont f;
            f.setStrikeOut(true);
            return f;
        }
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
        if(o == Qt::Vertical) {
            if(m_state.value(s) == Inserted) return QStringLiteral("＋");
            if(m_state.value(s) == Deleted)  return QStringLiteral("✕");
            return s + 1;
        }
        return m_cols.value(s);
    }

    Qt::ItemFlags flags(const QModelIndex &idx) const override
    {
        Qt::ItemFlags f = QAbstractTableModel::flags(idx);
        if(idx.isValid() && m_state.value(idx.row()) == Deleted)
            return f & ~Qt::ItemIsEditable;
        return f | Qt::ItemIsEditable;
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
    QVector<RowState>    m_state;
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

    /* ---- view controls: WHERE filter + sort + SQLyog-style row window --- */
    auto *tools = new QWidget(this);
    m_whereEdit = new QLineEdit(tools);
    m_whereEdit->setPlaceholderText(QStringLiteral("WHERE …   (Enter to filter)"));
    m_whereEdit->setClearButtonEnabled(true);
    connect(m_whereEdit, &QLineEdit::returnPressed, this,
            &TableDataView::applyViewControls);

    m_sortCombo = new QComboBox(tools);
    m_sortCombo->setMinimumContentsLength(12);
    connect(m_sortCombo, &QComboBox::activated, this,
            &TableDataView::applyViewControls);
    m_sortDirBtn = new QToolButton(tools);
    m_sortDirBtn->setText(QStringLiteral("▲"));
    m_sortDirBtn->setToolTip(QStringLiteral("Sort ascending / descending"));
    connect(m_sortDirBtn, &QToolButton::clicked, this, [this] {
        m_sortDesc = !m_sortDesc;
        m_sortDirBtn->setText(m_sortDesc ? QStringLiteral("▼")
                                         : QStringLiteral("▲"));
        applyViewControls();
    });

    /* SQLyog: [x] Limit rows   First row [0] ▶   # of rows [1000] */
    m_limitChk = new QCheckBox(QStringLiteral("Limit rows"), tools);
    m_limitChk->setChecked(true);
    m_limitChk->setToolTip(
        QStringLiteral("Off: fetch every matching row (may be slow)"));

    m_firstRow = new QSpinBox(tools);
    m_firstRow->setRange(0, 2000000000);
    m_firstRow->setSingleStep(1000);
    m_firstRow->setToolTip(
        QStringLiteral("First row — 0-based OFFSET; press Enter to apply"));
    connect(m_firstRow, &QSpinBox::editingFinished, this, [this] {
        if(m_valid && discardStagedEdits(QStringLiteral("Re-query")))
            reload();
    });

    m_nextBtn = new QToolButton(tools);
    m_nextBtn->setArrowType(Qt::RightArrow);
    m_nextBtn->setToolTip(QStringLiteral("Next window — advance by # of rows"));
    connect(m_nextBtn, &QToolButton::clicked, this, [this] { pageStep(1); });

    m_rowCount = new QSpinBox(tools);
    m_rowCount->setRange(1, 10000000);
    m_rowCount->setValue(1000);
    m_rowCount->setToolTip(QStringLiteral("# of rows — LIMIT; press Enter"));
    connect(m_rowCount, &QSpinBox::editingFinished, this, [this] {
        m_firstRow->setSingleStep(qMax(1, m_rowCount->value()));
        if(m_valid && discardStagedEdits(QStringLiteral("Re-query"))) {
            m_firstRow->blockSignals(true);
            m_firstRow->setValue(0);
            m_firstRow->blockSignals(false);
            reload();
        }
    });

    connect(m_limitChk, &QCheckBox::toggled, this, [this](bool on) {
        m_firstRow->setEnabled(on);
        m_nextBtn->setEnabled(on);
        m_rowCount->setEnabled(on);
        if(m_valid && discardStagedEdits(QStringLiteral("Re-query")))
            reload();
    });

    auto *refreshBtn = new QToolButton(tools);
    refreshBtn->setText(QStringLiteral("⟳"));
    refreshBtn->setToolTip(QStringLiteral("Refresh"));
    connect(refreshBtn, &QToolButton::clicked, this, &TableDataView::refresh);

    auto *tl = new QHBoxLayout(tools);
    tl->setContentsMargins(3, 2, 3, 2);
    tl->setSpacing(3);
    tl->addWidget(m_whereEdit, 1);
    tl->addWidget(new QLabel(QStringLiteral("Sort:"), tools));
    tl->addWidget(m_sortCombo);
    tl->addWidget(m_sortDirBtn);
    tl->addSpacing(12);
    tl->addWidget(m_limitChk);
    tl->addWidget(new QLabel(QStringLiteral("First row:"), tools));
    tl->addWidget(m_firstRow);
    tl->addWidget(m_nextBtn);
    tl->addWidget(new QLabel(QStringLiteral("# of rows:"), tools));
    tl->addWidget(m_rowCount);
    tl->addSpacing(6);
    tl->addWidget(refreshBtn);
    m_tools = tools;

    m_model = new TableDataModel(this);
    m_grid  = new QTableView(this);
    m_grid->setModel(m_model);
    m_grid->horizontalHeader()->setStretchLastSection(true);
    m_grid->setAlternatingRowColors(true);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);

    /* SQLyog: click a column header to ORDER BY it (toggles ASC/DESC) */
    m_grid->horizontalHeader()->setSectionsClickable(true);
    m_grid->horizontalHeader()->setSortIndicatorShown(true);
    connect(m_grid->horizontalHeader(), &QHeaderView::sectionClicked, this,
            [this](int section) {
        if(!m_valid || section < 0 || section >= m_columns.size())
            return;
        if(!discardStagedEdits(QStringLiteral("Re-sort")))
            return;
        const QString col = m_columns[section];
        const QString pfx = QStringLiteral("`%1` ").arg(col);
        m_sortDesc = m_orderBy.startsWith(pfx) ? !m_sortDesc : false;
        m_orderBy = pfx + (m_sortDesc ? QStringLiteral("DESC")
                                      : QStringLiteral("ASC"));
        const int ci = m_sortCombo->findText(col);
        if(ci >= 0) {
            m_sortCombo->blockSignals(true);
            m_sortCombo->setCurrentIndex(ci);
            m_sortCombo->blockSignals(false);
        }
        m_sortDirBtn->setText(m_sortDesc ? QStringLiteral("▼")
                                         : QStringLiteral("▲"));
        m_grid->horizontalHeader()->setSortIndicator(
            section, m_sortDesc ? Qt::DescendingOrder : Qt::AscendingOrder);
        m_firstRow->blockSignals(true);
        m_firstRow->setValue(0);
        m_firstRow->blockSignals(false);
        reload();
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_label);
    layout->addWidget(m_tools);
    layout->addWidget(m_applyBar);
    layout->addWidget(m_grid, 1);

    connect(m_model, &TableDataModel::pendingChanged, this,
            &TableDataView::updateApplyBar);
    connect(m_grid, &QTableView::customContextMenuRequested, this,
            [this](const QPoint &pos) {
        if(!m_valid || !m_grid->indexAt(pos).isValid())
            return;
        const int ops = m_model->pendingOps();
        const int row = m_grid->currentIndex().row();
        const bool del = row >= 0
            && m_model->rowState(row) == TableDataModel::Deleted;
        QMenu menu(this);
        QAction *setNull = menu.addAction(QStringLiteral("Set Cell Value to &NULL"),
                                          this, &TableDataView::setCellNull);
        setNull->setEnabled(m_grid->currentIndex().isValid() && !del);
        menu.addSeparator();
        QAction *apply = menu.addAction(QStringLiteral("A&pply Changes"), this,
                                        &TableDataView::applyPendingEdits);
        QAction *revert = menu.addAction(QStringLiteral("Re&vert Changes"), this,
                                         &TableDataView::revertPendingEdits);
        apply->setEnabled(ops > 0);
        revert->setEnabled(ops > 0);
        QAction *bigEdit = menu.addAction(
            QStringLiteral("&Edit Cell in Text Editor…"), this,
            &TableDataView::editCellInTextEditor);
        bigEdit->setEnabled(m_grid->currentIndex().isValid() && !del);
        menu.addSeparator();
        menu.addAction(del ? QStringLiteral("&Undelete Row")
                           : QStringLiteral("Mark Row for &Deletion"),
                       this, &TableDataView::deleteSelectedRow);
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
    /* fresh table → drop the filter / sort / offset from the last one
     * ("Limit rows" and "# of rows" stay as the user left them, like SQLyog) */
    m_where.clear();
    m_orderBy.clear();
    m_sortDesc = false;
    if(m_whereEdit) m_whereEdit->clear();
    if(m_sortDirBtn) m_sortDirBtn->setText(QStringLiteral("▲"));
    if(m_firstRow) {
        m_firstRow->blockSignals(true);
        m_firstRow->setValue(0);
        m_firstRow->blockSignals(false);
    }
    reload();
}

bool TableDataView::discardStagedEdits(const QString &action)
{
    if(m_model->pendingOps() == 0)
        return true;
    if(QMessageBox::question(this, QStringLiteral("Staged edits"),
           QStringLiteral("There are unsaved staged edits. %1 anyway and "
                          "discard them?").arg(action)) != QMessageBox::Yes)
        return false;
    m_model->revertAll();
    return true;
}

void TableDataView::applyViewControls()
{
    if(!m_valid || !discardStagedEdits(QStringLiteral("Re-query")))
        return;
    m_where = m_whereEdit->text().trimmed();
    const int sc = m_sortCombo->currentIndex();
    m_orderBy = (sc <= 0)
        ? QString()
        : QStringLiteral("`%1` %2").arg(m_sortCombo->currentText(),
                                        m_sortDesc ? QStringLiteral("DESC")
                                                   : QStringLiteral("ASC"));
    if(m_firstRow) {
        m_firstRow->blockSignals(true);
        m_firstRow->setValue(0);          /* new filter/sort → back to the top */
        m_firstRow->blockSignals(false);
    }
    reload();
}

void TableDataView::pageStep(int delta)
{
    if(!m_valid || !m_firstRow
       || !discardStagedEdits(QStringLiteral("Re-query")))
        return;
    const long long rc = m_rowCount ? m_rowCount->value() : 1000;
    long long want = (long long)m_firstRow->value() + (long long)delta * rc;
    if(want < 0)
        want = 0;
    if(delta > 0 && m_totalRows > 0 && want >= m_totalRows)
        return;                           /* already showing the last window */
    m_firstRow->blockSignals(true);
    m_firstRow->setValue(int(qMin<long long>(want, m_firstRow->maximum())));
    m_firstRow->blockSignals(false);
    reload();
}

void TableDataView::rebuildSortCombo()
{
    const QString keep = m_sortCombo->currentText();
    m_sortCombo->blockSignals(true);
    m_sortCombo->clear();
    m_sortCombo->addItem(QStringLiteral("(unsorted)"));
    m_sortCombo->addItems(m_columns);
    const int i = m_sortCombo->findText(keep);
    m_sortCombo->setCurrentIndex(i > 0 ? i : 0);
    m_sortCombo->blockSignals(false);
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
    const int ops = m_model->pendingOps();
    const int edits = m_model->dirtyRows().size();
    const int ins = m_model->rowsWithState(TableDataModel::Inserted).size();
    const int del = m_model->rowsWithState(TableDataModel::Deleted).size();
    m_applyBar->setVisible(ops > 0);
    m_applyBtn->setEnabled(ops > 0);
    m_revertBtn->setEnabled(ops > 0);
    QStringList parts;
    if(edits) parts << QStringLiteral("%1 edited row(s)").arg(edits);
    if(ins)   parts << QStringLiteral("%1 new").arg(ins);
    if(del)   parts << QStringLiteral("%1 to delete").arg(del);
    m_pendingLabel->setText(parts.join(QStringLiteral(", ")));
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
    rebuildSortCombo();

    const QString qualified = QStringLiteral("`%1`.`%2`").arg(m_db, m_table);
    const QString whereSql = m_where.isEmpty()
        ? QString() : QStringLiteral(" WHERE ") + m_where;

    /* total matching rows, for the pager */
    m_totalRows = 0;
    if(m_conn && mysql_query(m_conn,
           (QStringLiteral("SELECT COUNT(*) FROM ") + qualified + whereSql)
               .toUtf8().constData()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            if(MYSQL_ROW r = mysql_fetch_row(res))
                m_totalRows = r[0] ? QString::fromUtf8(r[0]).toLongLong() : 0;
            mysql_free_result(res);
        }
    } else if(!m_where.isEmpty()) {
        emit statusMessage(QStringLiteral("filter rejected: %1")
                               .arg(QString::fromUtf8(mysql_error(m_conn))));
        return;                                  /* keep the current grid */
    }

    const bool limited = !m_limitChk || m_limitChk->isChecked();
    const long long rc = m_rowCount ? m_rowCount->value() : 1000;
    long long offset = m_firstRow ? m_firstRow->value() : 0;
    if(offset < 0)
        offset = 0;
    /* clamp "First row" to the last window that still has rows */
    if(limited && m_totalRows > 0 && offset >= m_totalRows)
        offset = ((m_totalRows - 1) / rc) * rc;
    if(m_firstRow && offset != m_firstRow->value()) {
        m_firstRow->blockSignals(true);
        m_firstRow->setValue(int(qMin<long long>(offset, m_firstRow->maximum())));
        m_firstRow->blockSignals(false);
    }

    QString sql = QStringLiteral("SELECT * FROM ") + qualified + whereSql;
    if(!m_orderBy.isEmpty())
        sql += QStringLiteral(" ORDER BY ") + m_orderBy;
    if(limited)
        sql += QStringLiteral(" LIMIT %1 OFFSET %2").arg(rc).arg(offset);

    QStringList header;
    QVector<QStringList> rows;
    if(m_conn && mysql_query(m_conn, sql.toUtf8().constData()) == 0) {
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
    } else {
        emit statusMessage(QStringLiteral("query failed: %1")
                               .arg(QString::fromUtf8(mysql_error(m_conn))));
        return;
    }
    m_model->setGrid(header, rows);
    m_valid = true;

    const long long from = rows.isEmpty() ? 0 : offset + 1;
    const long long to = offset + rows.size();
    if(m_nextBtn)
        m_nextBtn->setEnabled(limited && to < m_totalRows);

    m_label->setText(QStringLiteral("%1.%2  —  rows %3–%4 of %5%6%7")
        .arg(m_db, m_table).arg(from).arg(to).arg(m_totalRows)
        .arg(m_where.isEmpty() ? QString()
                               : QStringLiteral("   [filtered]"))
        .arg(m_hasPrimary ? QString()
                          : QStringLiteral("   ⚠ no primary key")));
    emit statusMessage(
        QStringLiteral("%1.%2 — rows %3–%4 of %5")
            .arg(m_db, m_table).arg(from).arg(to).arg(m_totalRows));
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
    const QList<int> edited = m_model->dirtyRows();
    const QList<int> inserted = m_model->rowsWithState(TableDataModel::Inserted);
    const QList<int> deleted = m_model->rowsWithState(TableDataModel::Deleted);
    if(edited.isEmpty() && inserted.isEmpty() && deleted.isEmpty())
        return;

    const auto fail = [&](const QString &what) {
        const QString err = QString::fromUtf8(mysql_error(m_conn));
        mysql_query(m_conn, "ROLLBACK");
        emit statusMessage(QStringLiteral("Apply failed on %1 (rolled back): %2")
                               .arg(what, err));
    };
    const QByteArray db = m_db.toUtf8(), tbl = m_table.toUtf8();

    mysql_query(m_conn, "START TRANSACTION");

    /* 1. deletes (WHERE from the row's original values) */
    for(int r : deleted) {
        wyString q;
        q.Sprintf("DELETE FROM `%s`.`%s` WHERE %s LIMIT 1", db.constData(),
                  tbl.constData(), whereFromOrigRow(r).toUtf8().constData());
        if(mysql_query(m_conn, q.GetString()) != 0)
            return fail(QStringLiteral("DELETE"));
    }

    /* 2. inserts (only columns the user filled in) */
    for(int r : inserted) {
        QStringList names, vals;
        for(int c = 0; c < m_columns.size(); ++c) {
            const QString v = m_model->cur(r, c);
            if(v.isEmpty())
                continue;
            names << QStringLiteral("`%1`").arg(m_columns[c]);
            vals  << (v == QStringLiteral("NULL") ? QStringLiteral("NULL")
                                                  : quoteValue(v));
        }
        wyString q;
        if(names.isEmpty())
            q.Sprintf("INSERT INTO `%s`.`%s` () VALUES ()", db.constData(),
                      tbl.constData());
        else
            q.Sprintf("INSERT INTO `%s`.`%s` (%s) VALUES (%s)", db.constData(),
                      tbl.constData(),
                      names.join(QStringLiteral(", ")).toUtf8().constData(),
                      vals.join(QStringLiteral(", ")).toUtf8().constData());
        if(mysql_query(m_conn, q.GetString()) != 0)
            return fail(QStringLiteral("INSERT"));
    }

    /* 3. updates on edited existing rows */
    for(int r : edited) {
        QStringList setParts;
        for(int c = 0; c < m_columns.size(); ++c) {
            if(!m_model->dirty(r, c))
                continue;
            const QString v = m_model->cur(r, c);
            setParts << QStringLiteral("`%1` = %2")
                            .arg(m_columns[c],
                                 v == QStringLiteral("NULL") ? QStringLiteral("NULL")
                                                             : quoteValue(v));
        }
        const QString where = whereFromOrigRow(r);
        if(setParts.isEmpty() || where.isEmpty())
            continue;
        wyString q;
        q.Sprintf("UPDATE `%s`.`%s` SET %s WHERE %s LIMIT 1", db.constData(),
                  tbl.constData(),
                  setParts.join(QStringLiteral(", ")).toUtf8().constData(),
                  where.toUtf8().constData());
        if(mysql_query(m_conn, q.GetString()) != 0)
            return fail(QStringLiteral("UPDATE"));
    }

    mysql_query(m_conn, "COMMIT");
    emit statusMessage(QStringLiteral(
        "Applied: %1 updated, %2 inserted, %3 deleted")
        .arg(edited.size()).arg(inserted.size()).arg(deleted.size()));
    reload();   /* refresh — picks up AUTO_INCREMENT / trigger effects */
}

void TableDataView::revertPendingEdits()
{
    m_model->revertAll();
    emit statusMessage(QStringLiteral("Reverted staged changes"));
}

void TableDataView::deleteSelectedRow()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid())
        return;
    m_model->toggleDeleted(idx.row());   /* staged — commit with Apply */
}

void TableDataView::addRow()
{
    if(!m_valid)
        return;
    const int r = m_model->stageNewRow();
    m_grid->setCurrentIndex(m_model->index(r, 0));
    m_grid->edit(m_model->index(r, 0));   /* jump straight into editing */
}

void TableDataView::setCellNull()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid())
        return;
    m_model->stage(idx.row(), idx.column(), QStringLiteral("NULL"));
}

QByteArray TableDataView::fetchCellBytes(int row, int col) const
{
    QByteArray out;
    if(!m_conn || m_columns.isEmpty())
        return out;
    const QString where = whereFromOrigRow(row);
    if(where.isEmpty())
        return out;
    wyString q;
    q.Sprintf("SELECT `%s` FROM `%s`.`%s` WHERE %s LIMIT 1",
              m_columns[col].toUtf8().constData(), m_db.toUtf8().constData(),
              m_table.toUtf8().constData(), where.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) != 0)
        return out;
    if(MYSQL_RES *res = mysql_store_result(m_conn)) {
        if(MYSQL_ROW r = mysql_fetch_row(res)) {
            unsigned long *len = mysql_fetch_lengths(res);
            if(r[0] && len)
                out = QByteArray(r[0], int(len[0]));
        }
        mysql_free_result(res);
    }
    return out;
}

void TableDataView::editCellInTextEditor()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid())
        return;
    const int row = idx.row(), c = idx.column();
    const QString col = m_model->columns().value(c);
    const bool isNull = idx.data().toString() == QStringLiteral("NULL");

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Edit `%1` — row %2").arg(col).arg(row + 1));
    dlg.resize(600, 420);

    auto *tabs = new QTabWidget(&dlg);
    auto *edit = new QPlainTextEdit(tabs);
    edit->setPlainText(isNull ? QString() : idx.data().toString());
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tabs->addTab(edit, QStringLiteral("Text"));

    auto *hex = new QPlainTextEdit(tabs);
    hex->setReadOnly(true);
    hex->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    hex->setLineWrapMode(QPlainTextEdit::NoWrap);
    tabs->addTab(hex, QStringLiteral("Hex"));
    bool hexLoaded = false;
    connect(tabs, &QTabWidget::currentChanged, &dlg, [&](int i) {
        if(i != 1 || hexLoaded)
            return;
        hexLoaded = true;
        const QByteArray b = m_model->rowState(row) == TableDataModel::Inserted
            ? edit->toPlainText().toUtf8()
            : fetchCellBytes(row, c);
        QString dump;
        for(int off = 0; off < b.size(); off += 16) {
            QString h, a;
            for(int j = 0; j < 16; ++j) {
                if(off + j < b.size()) {
                    const uchar ch = uchar(b[off + j]);
                    h += QStringLiteral("%1 ").arg(ch, 2, 16, QLatin1Char('0'));
                    a += (ch >= 0x20 && ch < 0x7f) ? QChar(ch) : QLatin1Char('.');
                } else {
                    h += QStringLiteral("   ");
                }
            }
            dump += QStringLiteral("%1  %2 %3\n")
                        .arg(off, 8, 16, QLatin1Char('0')).arg(h, a);
        }
        hex->setPlainText(b.isEmpty() ? QStringLiteral("(empty / NULL)") : dump);
        hex->appendPlainText(QStringLiteral("\n%1 byte(s)").arg(b.size()));
    });

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    auto *nullBtn = buttons->addButton(QStringLiteral("Set &NULL"),
                                       QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    bool toNull = false;
    connect(nullBtn, &QPushButton::clicked, &dlg, [&] { toNull = true; dlg.accept(); });

    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(tabs, 1);
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return;
    /* only the Text tab is editable; the Hex view is read-only */
    m_model->stage(row, c, toNull ? QStringLiteral("NULL") : edit->toPlainText());
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
