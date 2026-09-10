#include "TableDataView.h"
#include "Icons.h"
#include "wyString.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QFont>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLineEdit>
#include <QMenu>
#include <QFontDatabase>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTabWidget>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <functional>

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

/* ---------------- row-select checkbox column ----------------------------- *
 * SQLyog's leftmost grid column is a checkbox (DataView tracks m_checkcount
 * and copy / export / delete act on the checked rows).  A vertical header
 * that paints a checkbox beside each row number gives the same behaviour
 * without shifting every data column by one.                               */

class RowCheckHeader : public QHeaderView
{
    Q_OBJECT
public:
    explicit RowCheckHeader(QWidget *parent = nullptr)
        : QHeaderView(Qt::Vertical, parent)
    {
        setSectionsClickable(true);
        setSectionResizeMode(QHeaderView::Fixed);
        setDefaultSectionSize(24);
        setFixedWidth(34);
    }

    QList<int> checkedRows() const
    {
        QList<int> out(m_checked.cbegin(), m_checked.cend());
        std::sort(out.begin(), out.end());
        return out;
    }

    void clearChecks()
    {
        if(m_checked.isEmpty())
            return;
        m_checked.clear();
        viewport()->update();
        emit checkedChanged();
    }

    void setAllChecked(bool on)
    {
        m_checked.clear();
        if(on && model())
            for(int r = 0; r < model()->rowCount(); ++r)
                m_checked.insert(r);
        viewport()->update();
        emit checkedChanged();
    }

    void setRowChecked(int row, bool on)          /* selftest helper */
    {
        if(on) m_checked.insert(row);
        else   m_checked.remove(row);
        viewport()->update();
        emit checkedChanged();
    }

signals:
    void checkedChanged();

protected:
    void paintSection(QPainter *p, const QRect &rect, int logical) const override
    {
        QHeaderView::paintSection(p, rect, logical);      /* bg + row number */
        QStyleOptionButton o;
        o.rect  = checkboxRect(rect);
        o.state = QStyle::State_Enabled
                | (m_checked.contains(logical) ? QStyle::State_On
                                               : QStyle::State_Off);
        style()->drawPrimitive(QStyle::PE_IndicatorCheckBox, &o, p);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        const int logical = logicalIndexAt(e->pos());
        if(logical >= 0) {
            const int y = sectionViewportPosition(logical);
            const QRect sec(0, y, width(), sectionSize(logical));
            if(checkboxRect(sec).adjusted(-3, -3, 3, 3).contains(e->pos())) {
                if(!m_checked.remove(logical))
                    m_checked.insert(logical);
                viewport()->update();
                emit checkedChanged();
                return;                        /* don't start a row selection */
            }
        }
        QHeaderView::mousePressEvent(e);
    }

private:
    static QRect checkboxRect(const QRect &sec)
    {
        const int sz = 13;
        return QRect(sec.left() + 3, sec.center().y() - sz / 2, sz, sz);
    }

    QSet<int> m_checked;
};

/* ---------------- the view ---------------- */

TableDataView::TableDataView(QWidget *parent)
    : QWidget(parent)
{
    m_label = new QLabel(
        QStringLiteral("Double-click a table in the Object Browser to open it."),
        this);
    m_label->setStyleSheet(QStringLiteral(
        "color: palette(mid); padding: 3px 6px;"));
    m_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    { QFont f = m_label->font(); f.setPointSizeF(f.pointSizeF() - 0.5);
      m_label->setFont(f); }

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

    /* ---- toolbar strip, laid out like SQLyog's "2 Table Data" pane ------
     * SQLyog's strip is near-frameless: flat 22 px buttons packed tight on
     * the plain window ground, one hairline rule under it. */
    auto *tools = new QWidget(this);
    tools->setObjectName(QStringLiteral("tdvTools"));
    tools->setStyleSheet(QStringLiteral(
        "#tdvTools QToolButton { border: none; padding: 2px; border-radius: 3px; }"
        "#tdvTools QToolButton:hover { background: palette(midlight); }"
        "#tdvTools QToolButton:pressed,"
        "#tdvTools QToolButton:checked { background: palette(highlight); }"
        /* the container QSS otherwise renders the plain line edit shorter than
         * the native spin boxes beside it — give it a matching box */
        "#tdvTools QLineEdit { border: 1px solid palette(mid); border-radius: 3px; "
        "padding: 3px 22px 3px 6px; background: palette(base); }"));

    /* helpers: authentic SQLyog bitmap (include/bitmaps) with a QStyle fallback,
     * and a flat tool button */
    const auto ico = [this](const QString &file, QStyle::StandardPixmap fb) {
        QIcon i = Icons::get(file);
        return i.isNull() ? style()->standardIcon(fb) : i;
    };
    const auto mkTool = [tools](const QIcon &ic, const QString &tip) {
        auto *b = new QToolButton(tools);
        b->setAutoRaise(true);
        b->setIcon(ic);
        b->setIconSize(QSize(16, 16));
        b->setToolTip(tip);
        return b;
    };

    /* left group, in SQLyog's ResultView::AddToolButtons order (command[] +
     * command2[]):
     *   [export] [copy ▾] │ [insert] [duplicate] [save] [delete] [revert]
     *                     │ [grid] [form] [text]                            */
    auto *btnExport = mkTool(ico(QStringLiteral("export_data.ico"),
                                 QStyle::SP_DialogSaveButton),
                             QStringLiteral("Export table data as CSV…"));
    connect(btnExport, &QToolButton::clicked, this, [this] { exportRows(); });

    /* SQLyog's copy button is a BTNS_WHOLEDROPDOWN — the whole button opens
     * the menu, a small arrow, no split divider.  InstantPopup matches it. */
    auto *btnCopy = mkTool(ico(QStringLiteral("copy_data.ico"), QStyle::SP_FileIcon),
                           QStringLiteral("Copy rows to the clipboard"));
    btnCopy->setPopupMode(QToolButton::InstantPopup);
    {
        auto *m = new QMenu(btnCopy);
        m->addAction(QStringLiteral("Copy rows (tab-separated)"),
                     this, [this] { copyRows(false); });
        m->addAction(QStringLiteral("Copy rows with column names"),
                     this, [this] { copyRows(true); });
        btnCopy->setMenu(m);
    }

    /* insert is a plain button upstream (TBSTYLE_BUTTON); "insert with values"
     * lives in the right-click menu, like SQLyog */
    auto *btnAdd = mkTool(ico(QStringLiteral("result_insert.ico"),
                              QStyle::SP_FileDialogNewFolder),
                          QStringLiteral("Insert row"));
    connect(btnAdd, &QToolButton::clicked, this, &TableDataView::addRow);

    auto *btnDup = mkTool(ico(QStringLiteral("duplicaterow.ico"),
                              QStyle::SP_FileDialogDetailedView),
                          QStringLiteral("Duplicate current row"));
    connect(btnDup, &QToolButton::clicked, this, &TableDataView::duplicateRow);

    m_tbApply = mkTool(ico(QStringLiteral("result_save.ico"),
                           QStyle::SP_DialogSaveButton),
                       QStringLiteral("Save staged changes (Apply)"));
    connect(m_tbApply, &QToolButton::clicked, this,
            &TableDataView::applyPendingEdits);

    m_tbDelRow = mkTool(ico(QStringLiteral("result_delete.ico"),
                            QStyle::SP_TrashIcon),
                        QStringLiteral("Mark current row for deletion"));
    connect(m_tbDelRow, &QToolButton::clicked, this,
            &TableDataView::deleteSelectedRow);

    m_tbRevert = mkTool(ico(QStringLiteral("result_cancel.ico"),
                            QStyle::SP_DialogResetButton),
                        QStringLiteral("Discard staged changes (Revert)"));
    connect(m_tbRevert, &QToolButton::clicked, this,
            &TableDataView::revertPendingEdits);

    m_tbApply->setEnabled(false);
    m_tbRevert->setEnabled(false);

    /* view toggles — exclusive & checkable, like SQLyog's grid / form / text.
     * Form view ships only in SQLyog Ultimate, so the button is present but
     * disabled (Community merely pops the upgrade dialog on it). */
    auto *viewGrp = new QButtonGroup(this);
    viewGrp->setExclusive(true);
    const auto mkView = [&](const QString &file, QStyle::StandardPixmap fb,
                            const QString &tip, int mode) {
        QToolButton *b = mkTool(ico(file, fb), tip);
        b->setCheckable(true);
        viewGrp->addButton(b, mode);
        return b;
    };
    m_tbGrid = mkView(QStringLiteral("grid_view.ico"),
                      QStyle::SP_FileDialogListView,
                      QStringLiteral("Grid view"), 0);
    m_tbForm = mkView(QStringLiteral("form_view.ico"),
                      QStyle::SP_FileDialogInfoView,
                      QStringLiteral("Form view — a SQLyog Ultimate feature"), 1);
    m_tbText = mkView(QStringLiteral("text_View.ico"),
                      QStyle::SP_FileDialogContentsView,
                      QStringLiteral("Text view — column-aligned dump"), 2);
    m_tbGrid->setChecked(true);
    m_tbForm->setEnabled(false);
    connect(viewGrp, &QButtonGroup::idClicked, this, &TableDataView::setViewMode);

    /* right group: filter · refresh │ [x] Limit rows …  (SQLyog keeps refresh
     * over here, next to the filter funnel — not with the DML icons) */
    QToolButton *btnFilter = nullptr;
    {
        QIcon fn = Icons::get(QStringLiteral("filter.ico"));
        if(fn.isNull())
            fn = QIcon::fromTheme(QStringLiteral("view-filter"));
        if(!fn.isNull()) {
            btnFilter = mkTool(fn, QStringLiteral("Apply the WHERE filter"));
            connect(btnFilter, &QToolButton::clicked, this,
                    &TableDataView::applyViewControls);
        }
    }
    auto *btnRefresh = mkTool(ico(QStringLiteral("refresh.ico"),
                                  QStyle::SP_BrowserReload),
                              QStringLiteral("Refresh data"));
    connect(btnRefresh, &QToolButton::clicked, this, &TableDataView::refresh);

    m_whereEdit = new QLineEdit(tools);
    m_whereEdit->setPlaceholderText(
        QStringLiteral("Filter:  WHERE clause — press Enter"));
    m_whereEdit->setClearButtonEnabled(true);
    m_whereEdit->setMinimumWidth(200);
    m_whereEdit->setMaximumWidth(340);
    connect(m_whereEdit, &QLineEdit::returnPressed, this,
            &TableDataView::applyViewControls);

    /* SQLyog right group: [x] Limit rows   First row [0] ▶   # of rows [1000] */
    m_limitChk = new QCheckBox(QStringLiteral("Limit rows"), tools);
    m_limitChk->setChecked(true);
    m_limitChk->setToolTip(
        QStringLiteral("Off: fetch every matching row (may be slow)"));

    m_firstRow = new QSpinBox(tools);
    m_firstRow->setRange(0, 2000000000);
    m_firstRow->setSingleStep(1000);
    m_firstRow->setMinimumWidth(78);
    m_firstRow->setAlignment(Qt::AlignRight);
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
    m_rowCount->setMinimumWidth(78);
    m_rowCount->setAlignment(Qt::AlignRight);
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

    /* keep the filter box the same height as the native spin boxes beside it */
    m_whereEdit->setMinimumHeight(m_rowCount->sizeHint().height());

    /* short hairline separator — a 16 px tick, not a full-height rule */
    const auto vsep = [tools] {
        auto *f = new QFrame(tools);
        f->setFrameShape(QFrame::VLine);
        f->setFrameShadow(QFrame::Plain);
        f->setFixedHeight(16);
        f->setStyleSheet(QStringLiteral("color: palette(mid);"));
        return f;
    };

    auto *tl = new QHBoxLayout(tools);
    tl->setContentsMargins(3, 1, 5, 1);
    tl->setSpacing(1);
    tl->addWidget(btnExport);
    tl->addWidget(btnCopy);
    tl->addWidget(vsep());
    tl->addWidget(btnAdd);
    tl->addWidget(btnDup);
    tl->addWidget(m_tbApply);
    tl->addWidget(m_tbDelRow);
    tl->addWidget(m_tbRevert);
    tl->addWidget(vsep());
    tl->addWidget(m_tbGrid);
    tl->addWidget(m_tbForm);
    tl->addWidget(m_tbText);
    tl->addStretch(1);
    tl->addWidget(m_whereEdit);
    if(btnFilter)
        tl->addWidget(btnFilter);
    tl->addWidget(btnRefresh);
    tl->addWidget(vsep());
    tl->addWidget(m_limitChk);
    tl->addSpacing(4);
    tl->addWidget(new QLabel(QStringLiteral("First row"), tools));
    tl->addWidget(m_firstRow);
    tl->addWidget(m_nextBtn);
    tl->addSpacing(4);
    tl->addWidget(new QLabel(QStringLiteral("# of rows"), tools));
    tl->addWidget(m_rowCount);
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
            &TableDataView::sortByColumn);

    /* row-select checkbox column on the left (SQLyog's leftmost grid column) */
    m_checkHeader = new RowCheckHeader(m_grid);
    m_grid->setVerticalHeader(m_checkHeader);
    connect(m_checkHeader, &RowCheckHeader::checkedChanged, this, [this] {
        const int n = m_checkHeader->checkedRows().size();
        emit statusMessage(n ? QStringLiteral("%1 row(s) checked").arg(n)
                             : QStringLiteral("row selection cleared"));
    });

    /* read-only text rendering (SQLyog's TEXT view — a Scintilla box upstream) */
    m_textView = new QPlainTextEdit(this);
    m_textView->setReadOnly(true);
    m_textView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_textView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_viewStack = new QStackedWidget(this);
    m_viewStack->addWidget(m_grid);       /* index 0 — grid */
    m_viewStack->addWidget(m_textView);   /* index 1 — text */

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);        /* bars sit flush against the grid, no gap band */
    layout->addWidget(m_tools);
    layout->addWidget(m_applyBar);
    layout->addWidget(m_viewStack, 1);
    layout->addWidget(m_label);   /* row-count caption sits under the grid, SQLyog-style */

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
        const int nchecked = checkedRows().size();
        menu.addAction(nchecked > 0
                           ? QStringLiteral("Mark %1 Checked Row(s) for &Deletion")
                                 .arg(nchecked)
                           : (del ? QStringLiteral("&Undelete Row")
                                  : QStringLiteral("Mark Row for &Deletion")),
                       this, &TableDataView::deleteSelectedRow);
        menu.addAction(QStringLiteral("&Add Row"), this, &TableDataView::addRow);
        menu.addAction(QStringLiteral("Add Row (with &values…)"), this,
                       &TableDataView::insertRowWithValues);
        menu.addSeparator();
        menu.addAction(QStringLiteral("&Check All Rows"), this,
                       [this] { checkAllRows(true); });
        QAction *uncheck = menu.addAction(QStringLiteral("&Uncheck All Rows"), this,
                                          [this] { checkAllRows(false); });
        uncheck->setEnabled(nchecked > 0);
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
    m_sortColumn = -1;
    m_sortDesc = false;
    if(m_whereEdit) m_whereEdit->clear();
    if(m_grid)
        m_grid->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
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

/* WHERE-filter box: Enter re-queries; the header-click sort is left intact */
void TableDataView::applyViewControls()
{
    if(!m_valid || !discardStagedEdits(QStringLiteral("Re-query")))
        return;
    m_where = m_whereEdit->text().trimmed();
    if(m_firstRow) {
        m_firstRow->blockSignals(true);
        m_firstRow->setValue(0);          /* new filter → back to the top */
        m_firstRow->blockSignals(false);
    }
    reload();
}

/* SQLyog: clicking a column header sorts by it; re-clicking flips ASC/DESC */
void TableDataView::sortByColumn(int section)
{
    if(!m_valid || section < 0 || section >= m_columns.size())
        return;
    if(!discardStagedEdits(QStringLiteral("Re-sort")))
        return;
    m_sortDesc = (section == m_sortColumn) ? !m_sortDesc : false;
    m_sortColumn = section;
    m_orderBy = QStringLiteral("`%1` %2")
        .arg(m_columns[section],
             m_sortDesc ? QStringLiteral("DESC") : QStringLiteral("ASC"));
    m_grid->horizontalHeader()->setSortIndicator(
        section, m_sortDesc ? Qt::DescendingOrder : Qt::AscendingOrder);
    if(m_firstRow) {
        m_firstRow->blockSignals(true);
        m_firstRow->setValue(0);
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
    if(m_checkHeader) m_checkHeader->clearChecks();
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
    if(m_tbApply)  m_tbApply->setEnabled(ops > 0);
    if(m_tbRevert) m_tbRevert->setEnabled(ops > 0);
    QStringList parts;
    if(edits) parts << QStringLiteral("%1 edited row(s)").arg(edits);
    if(ins)   parts << QStringLiteral("%1 new").arg(ins);
    if(del)   parts << QStringLiteral("%1 to delete").arg(del);
    m_pendingLabel->setText(parts.join(QStringLiteral(", ")));
}

/* ---- Grid / Form / Text view toggle (SQLyog DataView::SwitchView) --------- */

void TableDataView::setViewMode(int mode)
{
    if(mode == 1)                       /* Form — Ultimate only; ignore */
        return;
    m_viewMode = mode;
    if(mode == 2) {
        m_textView->setPlainText(renderTextView());
        m_viewStack->setCurrentWidget(m_textView);
    } else {
        m_viewStack->setCurrentWidget(m_grid);
    }
    if(m_tbGrid) m_tbGrid->setChecked(mode == 0);
    if(m_tbText) m_tbText->setChecked(mode == 2);
}

void TableDataView::refreshTextViewIfShown()
{
    if(m_viewMode == 2 && m_textView)
        m_textView->setPlainText(renderTextView());
}

/* ---- row-select checkbox column -------------------------------------- */

QList<int> TableDataView::checkedRows() const
{
    return m_checkHeader ? m_checkHeader->checkedRows() : QList<int>();
}

void TableDataView::checkAllRows(bool on)
{
    if(m_checkHeader)
        m_checkHeader->setAllChecked(on);
}

void TableDataView::checkRowsForTest(const QString &csv)
{
    if(!m_checkHeader)
        return;
    m_checkHeader->setAllChecked(false);
    for(const QString &tok : csv.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int r = tok.trimmed().toInt(&ok);
        if(ok)
            m_checkHeader->setRowChecked(r, true);
    }
}

/* column-aligned dump, à la upstream FormatResultSet: header row, a dashed
 * rule, then the rows — each column padded to max(name, widest value) + gutter */
QString TableDataView::renderTextView() const
{
    const QStringList cols = m_model->columns();
    const int nrows = m_model->rowCount();
    const int ncols = cols.size();
    if(ncols == 0)
        return QString();

    QVector<int> w(ncols);
    for(int c = 0; c < ncols; ++c) {
        w[c] = cols[c].length();
        for(int r = 0; r < nrows; ++r)
            w[c] = qMax(w[c], m_model->cur(r, c).length());
    }

    const auto padRow = [&](const QStringList &vals) {
        QString line;
        for(int c = 0; c < ncols; ++c) {
            QString cell = vals.value(c);
            cell.replace('\t', QLatin1Char(' '));
            cell.replace('\n', QLatin1Char(' '));
            line += cell.leftJustified(w[c], QLatin1Char(' '));
            if(c + 1 < ncols)
                line += QStringLiteral("  ");
        }
        return line;
    };

    QStringList out;
    out << padRow(cols);
    QStringList rule;
    for(int c = 0; c < ncols; ++c)
        rule << QString(w[c], QLatin1Char('-'));
    out << rule.join(QStringLiteral("  "));
    for(int r = 0; r < nrows; ++r) {
        QStringList vals;
        for(int c = 0; c < ncols; ++c)
            vals << m_model->cur(r, c);
        out << padRow(vals);
    }
    return out.join(QLatin1Char('\n'));
}

/* copy the selected rows (or all, if nothing is selected) as TSV */
void TableDataView::copyRows(bool withHeader)
{
    if(!m_valid)
        return;
    const QStringList cols = m_model->columns();
    /* checkbox column wins; then the QTableView selection; then every row */
    QList<int> rows = checkedRows();
    if(rows.isEmpty()) {
        const auto sel = m_grid->selectionModel()
                         ? m_grid->selectionModel()->selectedRows() : QModelIndexList();
        for(const QModelIndex &idx : sel)
            rows << idx.row();
    }
    if(rows.isEmpty())
        for(int r = 0; r < m_model->rowCount(); ++r)
            rows << r;
    std::sort(rows.begin(), rows.end());

    QStringList lines;
    if(withHeader)
        lines << cols.join(QLatin1Char('\t'));
    for(int r : rows) {
        QStringList vals;
        for(int c = 0; c < cols.size(); ++c)
            vals << m_model->cur(r, c);
        lines << vals.join(QLatin1Char('\t'));
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    emit statusMessage(QStringLiteral("Copied %1 row(s) to the clipboard")
                           .arg(rows.size()));
}

/* dump every loaded row to a CSV file (SQLyog's IDM_IMEX_EXPORTDATA, minus
 * the format picker — CSV only for now) */
void TableDataView::exportRows()
{
    if(!m_valid)
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export table data as CSV"),
        QStringLiteral("%1.csv").arg(m_table),
        QStringLiteral("CSV (*.csv);;All files (*)"));
    if(path.isEmpty())
        return;
    QFile f(path);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit statusMessage(QStringLiteral("cannot write %1").arg(path));
        return;
    }
    const auto csv = [](QString v) {
        if(v.contains(QLatin1Char('"')) || v.contains(QLatin1Char(','))
           || v.contains(QLatin1Char('\n')))
            return QLatin1Char('"') + v.replace(QLatin1Char('"'),
                                                QStringLiteral("\"\"")) + QLatin1Char('"');
        return v;
    };
    const QStringList cols = m_model->columns();
    QList<int> rows = checkedRows();          /* checked rows only, else all */
    if(rows.isEmpty())
        for(int r = 0; r < m_model->rowCount(); ++r)
            rows << r;
    QTextStream out(&f);
    QStringList head;
    for(const QString &c : cols)
        head << csv(c);
    out << head.join(QLatin1Char(',')) << '\n';
    for(int r : rows) {
        QStringList vals;
        for(int c = 0; c < cols.size(); ++c)
            vals << csv(m_model->cur(r, c));
        out << vals.join(QLatin1Char(',')) << '\n';
    }
    emit statusMessage(QStringLiteral("Exported %1 row(s) → %2")
                           .arg(rows.size()).arg(path));
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
    if(m_checkHeader) m_checkHeader->clearChecks();
    m_valid = true;

    /* keep the header's sort arrow in sync after the model reset */
    m_grid->horizontalHeader()->setSortIndicator(
        m_sortColumn,
        m_sortDesc ? Qt::DescendingOrder : Qt::AscendingOrder);

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

    refreshTextViewIfShown();
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
    if(!m_valid)
        return;
    /* checked rows win: toggle-delete each.  Descending order so that an
     * Inserted row being removed outright doesn't shift the rows below it. */
    QList<int> chk = checkedRows();
    if(!chk.isEmpty()) {
        std::sort(chk.begin(), chk.end(), std::greater<int>());
        for(int r : chk)
            m_model->toggleDeleted(r);
        if(m_checkHeader)
            m_checkHeader->clearChecks();   /* indices may have shifted */
        return;
    }
    const QModelIndex idx = m_grid->currentIndex();
    if(!idx.isValid())
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

/* SQLyog "duplicate row": stage a new row pre-filled from the current one
 * (AUTO_INCREMENT columns left blank so the server assigns a fresh value) */
void TableDataView::duplicateRow()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid())
        return;
    const int src = idx.row();
    const int r = m_model->stageNewRow();
    for(int c = 0; c < m_columns.size(); ++c) {
        if(m_colInfo.value(c).autoInc)
            continue;
        m_model->stage(r, c, m_model->cur(src, c));
    }
    m_grid->setCurrentIndex(m_model->index(r, 0));
    emit statusMessage(
        QStringLiteral("Row %1 copied to a new staged row — Apply to insert")
            .arg(src + 1));
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
