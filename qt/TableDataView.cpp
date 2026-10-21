#include "TableDataView.h"
#include "TableDataModel.h"
#include "FormView.h"
#include "ExportDialog.h"
#include "Icons.h"
#include "db/IDbConnection.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QEvent>
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
#include <QMap>
#include <QMenu>
#include <QFontDatabase>
#include <QMessageBox>
#include <QRegularExpression>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionHeader>
#include <QTabWidget>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

/* ---------------- the view ---------------- */

TableDataView::TableDataView(QWidget *parent) : QWidget(parent)
{
    m_label =
        new QLabel(QStringLiteral("Double-click a table in the Object Browser to open it."), this);
    m_label->setStyleSheet(QStringLiteral("color: palette(mid); padding: 3px 6px;"));
    m_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    {
        QFont f = m_label->font();
        f.setPointSizeF(f.pointSizeF() - 0.5);
        m_label->setFont(f);
    }

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
    connect(m_applyBtn, &QPushButton::clicked, this, &TableDataView::applyPendingEdits);
    connect(m_revertBtn, &QPushButton::clicked, this, &TableDataView::revertPendingEdits);

    /* ---- toolbar strip, laid out like SQLyog's "2 Table Data" pane ------
     * SQLyog's strip is near-frameless: flat 22 px buttons packed tight on
     * the plain window ground, one hairline rule under it. */
    auto *tools = new QWidget(this);
    tools->setObjectName(QStringLiteral("tdvTools"));
    tools->setStyleSheet(
        QStringLiteral("#tdvTools QToolButton { border: none; padding: 2px; border-radius: 3px; }"
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
    auto *btnExport = mkTool(ico(QStringLiteral("export_data.ico"), QStyle::SP_DialogSaveButton),
                             QStringLiteral("Export table data…  (CSV / TSV / "
                                            "HTML / JSON / XML / SQL / Excel)"));
    connect(btnExport, &QToolButton::clicked, this, [this] { exportRows(); });

    /* SQLyog's copy button is a BTNS_WHOLEDROPDOWN — the whole button opens
     * the menu, a small arrow, no split divider.  InstantPopup matches it. */
    auto *btnCopy = mkTool(ico(QStringLiteral("copy_data.ico"), QStyle::SP_FileIcon),
                           QStringLiteral("Copy rows to the clipboard"));
    btnCopy->setPopupMode(QToolButton::InstantPopup);
    {
        auto *m = new QMenu(btnCopy);
        m->addAction(QStringLiteral("Copy rows (tab-separated)"), this,
                     [this] { copyRows(false); });
        m->addAction(QStringLiteral("Copy rows with column names"), this,
                     [this] { copyRows(true); });
        btnCopy->setMenu(m);
    }

    /* insert is a plain button upstream (TBSTYLE_BUTTON); "insert with values"
     * lives in the right-click menu, like SQLyog */
    auto *btnAdd = mkTool(ico(QStringLiteral("result_insert.ico"), QStyle::SP_FileDialogNewFolder),
                          QStringLiteral("Insert row"));
    connect(btnAdd, &QToolButton::clicked, this, &TableDataView::addRow);

    auto *btnDup =
        mkTool(ico(QStringLiteral("duplicaterow.ico"), QStyle::SP_FileDialogDetailedView),
               QStringLiteral("Duplicate current row"));
    connect(btnDup, &QToolButton::clicked, this, &TableDataView::duplicateRow);

    m_tbApply = mkTool(ico(QStringLiteral("result_save.ico"), QStyle::SP_DialogSaveButton),
                       QStringLiteral("Save staged changes (Apply)"));
    connect(m_tbApply, &QToolButton::clicked, this, &TableDataView::applyPendingEdits);

    m_tbDelRow = mkTool(ico(QStringLiteral("result_delete.ico"), QStyle::SP_TrashIcon),
                        QStringLiteral("Mark current row for deletion"));
    connect(m_tbDelRow, &QToolButton::clicked, this, &TableDataView::deleteSelectedRow);

    m_tbRevert = mkTool(ico(QStringLiteral("result_cancel.ico"), QStyle::SP_DialogResetButton),
                        QStringLiteral("Discard staged changes (Revert)"));
    connect(m_tbRevert, &QToolButton::clicked, this, &TableDataView::revertPendingEdits);

    m_tbApply->setEnabled(false);
    m_tbRevert->setEnabled(false);

    /* view toggles — exclusive & checkable, like SQLyog's grid / form / text */
    auto *viewGrp = new QButtonGroup(this);
    viewGrp->setExclusive(true);
    const auto mkView = [&](const QString &file, QStyle::StandardPixmap fb, const QString &tip,
                            int mode) {
        QToolButton *b = mkTool(ico(file, fb), tip);
        b->setCheckable(true);
        viewGrp->addButton(b, mode);
        return b;
    };
    m_tbGrid = mkView(QStringLiteral("grid_view.ico"), QStyle::SP_FileDialogListView,
                      QStringLiteral("Grid view"), 0);
    m_tbForm = mkView(QStringLiteral("form_view.ico"), QStyle::SP_FileDialogInfoView,
                      QStringLiteral("Form view — one row at a time"), 1);
    m_tbText = mkView(QStringLiteral("text_View.ico"), QStyle::SP_FileDialogContentsView,
                      QStringLiteral("Text view — column-aligned dump"), 2);
    m_tbGrid->setChecked(true);
    connect(viewGrp, &QButtonGroup::idClicked, this, &TableDataView::setViewMode);

    /* right group: filter · refresh │ [x] Limit rows …  (SQLyog keeps refresh
     * over here, next to the filter funnel — not with the DML icons).
     * Upstream (src/DataView.cpp's ID_RESETFILTER handler + UpdateFilterIcon())
     * uses ONE toolbar slot for this, not two: its icon and behavior swap
     * depending on whether a filter is currently active — the plain funnel
     * ("Custom Filter…", opens the dialog) when not, a funnel-with-a-red-X
     * ("Reset Filter", clears directly) once one is. First pass here had
     * two separate, always-visible buttons; consolidated after the owner
     * checked upstream's actual behavior mid-filter. */
    m_btnFilter = mkTool(ico(QStringLiteral("filter.ico"), QStyle::SP_FileDialogDetailedView),
                         QStringLiteral("Custom Filter…"));
    connect(m_btnFilter, &QToolButton::clicked, this,
            [this] { m_where.isEmpty() ? openCustomFilter() : resetFilter(); });

    auto *btnRefresh = mkTool(ico(QStringLiteral("refresh.ico"), QStyle::SP_BrowserReload),
                              QStringLiteral("Refresh data"));
    connect(btnRefresh, &QToolButton::clicked, this, &TableDataView::refresh);

    /* no separate "WHERE …" status text next to the funnel — owner: the
     * funnel button sits right beside Refresh, same as upstream, and the
     * button's own icon (plain funnel ⇄ funnel-with-X) already says
     * whether a filter is active; a text label alongside it was redundant */

    /* SQLyog right group: [x] Limit rows   First row [0] ▶   # of rows [1000] */
    m_limitChk = new QCheckBox(QStringLiteral("Limit rows"), tools);
    m_limitChk->setChecked(true);
    m_limitChk->setToolTip(QStringLiteral("Off: fetch every matching row (may be slow)"));

    m_firstRow = new QSpinBox(tools);
    m_firstRow->setRange(0, 2000000000);
    m_firstRow->setSingleStep(1000);
    m_firstRow->setMinimumWidth(78);
    m_firstRow->setAlignment(Qt::AlignRight);
    /* the app has no other localization yet, so a Bengali/etc. system
     * locale would otherwise render this in that locale's own digits
     * (e.g. bn_BD's ০১২…) while every other number in the UI stays
     * Latin — same fix as ConnectionDialog's/Copy Table's port fields */
    m_firstRow->setLocale(QLocale::c());
    m_firstRow->setToolTip(QStringLiteral("First row — 0-based OFFSET; press Enter to apply"));
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
    m_rowCount->setLocale(QLocale::c());
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
    tl->addWidget(m_btnFilter);
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
    m_grid = new QTableView(this);
    m_grid->setModel(m_model);
    m_grid->setAlternatingRowColors(true);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);

    /* SQLyog: click a column header to ORDER BY it (toggles ASC/DESC) */
    m_grid->horizontalHeader()->setSectionsClickable(true);
    m_grid->horizontalHeader()->setSortIndicatorShown(true);
    connect(m_grid->horizontalHeader(), &QHeaderView::sectionClicked, this,
            &TableDataView::sortByColumn);
    /* …and the same menu opens under the clicked header, aimed at that
     * column (current row's cell) so Filter/Set-To act on it. Deferred one
     * turn: sortByColumn() above reloads the grid first. */
    connect(m_grid->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int section) {
        QTimer::singleShot(0, this, [this, section] {
            if(!m_valid || section >= m_model->columnCount())
                return;
            const int row = m_grid->currentIndex().isValid() ? m_grid->currentIndex().row() : 0;
            const QModelIndex cell =
                row < m_model->rowCount() ? m_model->index(row, section) : QModelIndex();
            showContextMenu(QPoint(m_grid->horizontalHeader()->sectionViewportPosition(section), 0),
                            cell);
        });
    });

    /* row-select checkbox column on the left (SQLyog's leftmost grid column) */
    m_checkHeader = new RowCheckHeader(m_grid);
    m_grid->setVerticalHeader(m_checkHeader);
    m_grid->setCornerButtonEnabled(true);
    if(auto *corner = m_grid->findChild<QAbstractButton *>())
        m_checkHeader->attachCornerButton(corner); /* select-all checkbox */
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
    m_viewStack->addWidget(m_grid);     /* index 0 — grid */
    m_viewStack->addWidget(m_textView); /* index 1 — text */
    m_formView = new FormView(this);
    m_formView->setModel(
        m_model, {[this](int r) { return int(m_model->rowState(r)); },
                  [this](int r, int col) { return m_model->dirty(r, col); },
                  /* Form view BLOB preview/file-io: raw bytes via the same streamed
                   * re-query the Hex tab uses; staging back as a driver hex literal
                   * through the same stageExpr path its OK button uses */
                  [this](int r, int c) { return fetchCellBytes(r, c); },
                  [this](int r, int c, const QByteArray &bytes) {
                      const QString h = bytes.toHex();
                      const SqlDriverType drv =
                          m_conn ? m_conn->driverType() : SqlDriverType::Mysql;
                      const QString expr = hexLiteral(drv, h);
                      const QString disp = h.size() > 32
                                               ? hexLiteral(drv, h.left(32) + QStringLiteral("…")) +
                                                     QStringLiteral(" (%1 bytes)").arg(h.size() / 2)
                                               : expr;
                      m_model->stageExpr(r, c, disp, expr);
                  }});
    m_viewStack->addWidget(m_formView); /* index 2 — form */
    connect(m_formView, &FormView::newRowRequested, this, &TableDataView::formNewRow);
    connect(m_formView, &FormView::duplicateRequested, this, &TableDataView::formDuplicateRow);
    connect(m_formView, &FormView::deleteRequested, this, &TableDataView::formDeleteRow);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0); /* bars sit flush against the grid, no gap band */
    layout->addWidget(m_tools);
    layout->addWidget(m_applyBar);
    layout->addWidget(m_viewStack, 1);
    layout->addWidget(m_label); /* row-count caption sits under the grid, SQLyog-style */

    connect(m_model, &TableDataModel::pendingChanged, this, &TableDataView::updateApplyBar);
    connect(m_grid, &QTableView::customContextMenuRequested, this,
            [this](const QPoint &pos) { showContextMenu(pos); });

    m_clickMenuTimer = new QTimer(this);
    m_clickMenuTimer->setSingleShot(true);
    connect(m_clickMenuTimer, &QTimer::timeout, this, [this] {
        /* the menu is modal — skip it if the pointer has since left the grid */
        if(m_grid->viewport()->rect().contains(m_clickMenuPos))
            showContextMenu(m_clickMenuPos);
    });
    m_grid->viewport()->installEventFilter(this);
}

bool TableDataView::eventFilter(QObject *o, QEvent *e)
{
    if(o != m_grid->viewport())
        return QWidget::eventFilter(o, e);
    switch(e->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            m_clickMenuTimer->stop(); /* a second click means "edit", not "menu" */
            if(e->type() == QEvent::MouseButtonPress)
                m_pressPos = static_cast<QMouseEvent *>(e)->position().toPoint();
            break;
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(e);
            const QPoint pos = me->position().toPoint();
            /* plain left click on a data cell: not a drag-select, not a
             * Ctrl/Shift multi-select gesture, and not while an editor is open */
            if(m_valid && me->button() == Qt::LeftButton && me->modifiers() == Qt::NoModifier &&
               (pos - m_pressPos).manhattanLength() < QApplication::startDragDistance() &&
               m_grid->indexAt(pos).isValid() &&
               !m_grid->viewport()->isAncestorOf(QApplication::focusWidget())) {
                m_clickMenuPos = pos;
                m_clickMenuTimer->start(QApplication::doubleClickInterval() + 50);
            }
            break;
        }
        default:
            break;
    }
    return QWidget::eventFilter(o, e);
}

/* ---------------- grid context menu (upstream IDR_QUERYLISTMENU) ---------- */

void TableDataView::load(IDbConnection *conn, const QString &db, const QString &table)
{
    m_conn = conn;
    m_db = db;
    m_table = table;
    /* fresh table → drop the filter / sort / offset from the last one
     * ("Limit rows" and "# of rows" stay as the user left them, like SQLyog) */
    m_where.clear();
    m_filterRows.clear();
    if(m_btnFilter) {
        m_btnFilter->setIcon(Icons::get(QStringLiteral("filter.ico")));
        m_btnFilter->setToolTip(QStringLiteral("Custom Filter…"));
    }
    m_orderBy.clear();
    m_sortColumn = -1;
    m_sortDesc = false;
    if(m_grid)
        m_grid->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    if(m_firstRow) {
        m_firstRow->blockSignals(true);
        m_firstRow->setValue(0);
        m_firstRow->blockSignals(false);
    }
    reload();
}

/* funnel button: upstream's own Custom Filter dialog (src/SortAndFilter.cpp),
 * not a raw WHERE-clause text box — Field/Condition/Value rows, AND-joined */
void TableDataView::openCustomFilter()
{
    if(!m_valid)
        return;
    const auto qi = [this](const QString &ident) {
        return m_conn ? m_conn->quoteIdent(ident) : QStringLiteral("`%1`").arg(ident);
    };
    const auto esc = [this](const QString &v) {
        return m_conn ? QString::fromUtf8(m_conn->escape(v.toUtf8())) : v;
    };
    /* the same query reload() would actually run with this WHERE applied —
     * upstream's own preview shows the complete SELECT (IQueryBuilder::
     * GetQuery()), not just the WHERE fragment, so this one should too */
    const auto fullQuery = [this](const QString &where) {
        const QString qualified = m_conn ? m_conn->qualify(m_db, m_table)
                                         : QStringLiteral("`%1`.`%2`").arg(m_db, m_table);
        QString sql = QStringLiteral("SELECT * FROM ") + qualified;
        if(!where.isEmpty())
            sql += QStringLiteral(" WHERE ") + where;
        if(!m_orderBy.isEmpty())
            sql += QStringLiteral(" ORDER BY ") + m_orderBy;
        if(!m_limitChk || m_limitChk->isChecked())
            sql += QStringLiteral(" LIMIT %1 OFFSET %2")
                       .arg(m_rowCount ? m_rowCount->value() : 1000)
                       .arg(m_firstRow ? m_firstRow->value() : 0);
        return sql + QStringLiteral(";");
    };
    CustomFilterDialog dlg(m_columns, m_filterRows, qi, esc, fullQuery, this);
    if(dlg.exec() != QDialog::Accepted)
        return;
    if(!discardStagedEdits(QStringLiteral("Re-query")))
        return;
    m_filterRows = dlg.rows();
    applyFilterWhere(dlg.whereClause());
}

/* Reset Filter button: upstream's ID_RESETFILTER — clears the active
 * filter directly, no dialog. A no-op (harmless) when nothing was set. */
void TableDataView::resetFilter()
{
    if(!m_valid || (m_where.isEmpty() && m_filterRows.isEmpty()))
        return;
    if(!discardStagedEdits(QStringLiteral("Re-query")))
        return;
    m_filterRows.clear();
    applyFilterWhere(QString());
}

void TableDataView::applyFilterWhere(const QString &where)
{
    m_where = where;
    /* upstream's UpdateFilterIcon(): the one filter button's icon/tooltip
     * follow whether a filter is active, not two separate buttons. No
     * persistent "WHERE …" text next to it (owner: not needed) — the
     * active clause is still one hover away, on the button's own tooltip. */
    if(m_where.isEmpty()) {
        m_btnFilter->setIcon(Icons::get(QStringLiteral("filter.ico")));
        m_btnFilter->setToolTip(QStringLiteral("Custom Filter…"));
    } else {
        m_btnFilter->setIcon(Icons::get(QStringLiteral("resetfilter.ico")));
        m_btnFilter->setToolTip(QStringLiteral("Reset Filter — WHERE %1").arg(m_where));
    }
    if(m_firstRow) {
        m_firstRow->blockSignals(true);
        m_firstRow->setValue(0); /* new filter → back to the top */
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
    m_orderBy = QStringLiteral("%1 %2").arg(
        m_conn ? m_conn->quoteIdent(m_columns[section]) : m_columns[section],
        m_sortDesc ? QStringLiteral("DESC") : QStringLiteral("ASC"));
    m_grid->horizontalHeader()->setSortIndicator(section, m_sortDesc ? Qt::DescendingOrder
                                                                     : Qt::AscendingOrder);
    if(m_firstRow) {
        m_firstRow->blockSignals(true);
        m_firstRow->setValue(0);
        m_firstRow->blockSignals(false);
    }
    reload();
}

void TableDataView::pageStep(int delta)
{
    if(!m_valid || !m_firstRow || !discardStagedEdits(QStringLiteral("Re-query")))
        return;
    const long long rc = m_rowCount ? m_rowCount->value() : 1000;
    long long want = (long long)m_firstRow->value() + (long long)delta * rc;
    if(want < 0)
        want = 0;
    if(delta > 0 && m_totalRows > 0 && want >= m_totalRows)
        return; /* already showing the last window */
    m_firstRow->blockSignals(true);
    m_firstRow->setValue(int(qMin<long long>(want, m_firstRow->maximum())));
    m_firstRow->blockSignals(false);
    reload();
}

void TableDataView::clear()
{
    m_valid = false;
    m_model->setGrid({}, {});
    if(m_checkHeader)
        m_checkHeader->clearChecks();
    m_label->setText(QStringLiteral("Double-click a table in the Object Browser to open it."));
}

void TableDataView::hexCellForTest(int row, int col, const QString &hex)
{
    const QString h = hex.trimmed().toLower();
    const QString expr = hexLiteral(m_conn ? m_conn->driverType() : SqlDriverType::Mysql, h);
    m_model->stageExpr(row, col, expr, expr);
    applyPendingEdits();
}

QString TableDataView::cellTextForTest(int row, int col) const
{
    return m_model->data(m_model->index(row, col), Qt::DisplayRole).toString();
}

/* selftest: a Filter ▸ quick clause, end to end (discard-prompt is a no-op
 * with nothing staged) — asserts the rebuilt grid through the same reload()
 * the funnel button drives */
void TableDataView::quickFilterForTest(const QString &where)
{
    quickFilter(where);
}

/* selftest: "Set To Default" on one cell, then Apply — the UPDATE writes
 * the raw DEFAULT keyword and the reload shows the column's default */
void TableDataView::setCellDefaultForTest(int row, int col)
{
    m_grid->setCurrentIndex(m_model->index(row, col));
    setCellDefault();
    applyPendingEdits();
}

/* --tdmenu=row:col — build (without exec) the grid's context menu over that
 * cell and dump every item's label + enabled state, submenus flattened with
 * a ▸ marker, so the upstream-shape checks run headlessly */
QString TableDataView::contextMenuDumpForTest(int row, int col)
{
    if(!m_valid || row < 0 || row >= m_model->rowCount() || col < 0 || col >= m_columns.size())
        return QStringLiteral("tdmenu: bad cell\n");
    QMenu *menu = buildContextMenu(m_model->index(row, col));
    QString out;
    std::function<void(const QMenu *)> walk = [&](const QMenu *m) {
        for(QAction *a : m->actions()) {
            if(a->isSeparator())
                out += QStringLiteral("--\n");
            else if(a->menu()) {
                out += a->text() + QStringLiteral(" ▸\n");
                walk(a->menu());
            } else {
                out += a->text() +
                       (a->isEnabled() ? QStringLiteral("  [on]\n") : QStringLiteral("  [off]\n"));
            }
        }
    };
    walk(menu);
    menu->deleteLater();
    return out;
}

void TableDataView::checkRowsForTest(const QString &csv)
{
    if(!m_checkHeader)
        return;
    if(csv.trimmed() == QLatin1String("all")) {
        m_checkHeader->setAllChecked(true);
        return;
    }
    m_checkHeader->setAllChecked(false);
    for(const QString &tok : csv.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int r = tok.trimmed().toInt(&ok);
        if(ok)
            m_checkHeader->setRowChecked(r, true);
    }
}

void TableDataView::reload()
{
    /* columns + keys through the driver seam (canonical SHOW COLUMNS /
     * SHOW INDEX shapes — see qt/db/IDbConnection.h) */
    QStringList pkeys;
    if(m_conn) {
        const DbResultSet keys = m_conn->listIndexes(m_db, m_table);
        for(const QStringList &row : keys.rows) {
            if(row.value(1) == QStringLiteral("0") && row.value(2) == QStringLiteral("PRIMARY"))
                pkeys << row.value(4);
        }
    }

    m_columns.clear();
    m_colInfo.clear();
    const DbResultSet cols = m_conn ? m_conn->listColumns(m_db, m_table) : DbResultSet();
    for(const QStringList &row : cols.rows) {
        m_columns << row.value(0);
        ColumnInfo ci;
        ci.name = m_columns.last();
        ci.nullable = row.value(2) != QStringLiteral("NO");
        ci.autoInc = row.value(5).contains(QStringLiteral("auto_increment"));
        ci.blob = typeLooksBinary(row.value(1));
        /* Default(4): the seam's canonical SHOW COLUMNS shape encodes a
         * NULL default as the literal "NULL" — anything non-empty besides
         * that (including MySQL's DEFAULT '') means "Set To Default" can
         * run (upstream's iscolhasdefault gate) */
        ci.hasDefault = !row.value(4).isEmpty() && row.value(4) != QStringLiteral("NULL");
        ci.type = row.value(1);
        m_colInfo << ci;
    }
    if(m_columns.isEmpty()) {
        clear();
        emit statusMessage(QStringLiteral("cannot read columns of %1.%2").arg(m_db, m_table));
        return;
    }

    m_pkColumns.clear();
    for(int i = 0; i < m_columns.size(); ++i)
        if(pkeys.contains(m_columns[i]))
            m_pkColumns << i;
    m_hasPrimary = !m_pkColumns.isEmpty(); /* upstream: PK only, else all-cols */

    const QString qualified =
        m_conn ? m_conn->qualify(m_db, m_table) : QStringLiteral("`%1`.`%2`").arg(m_db, m_table);
    const QString whereSql = m_where.isEmpty() ? QString() : QStringLiteral(" WHERE ") + m_where;

    /* total matching rows, for the pager */
    m_totalRows = 0;
    {
        DbResultSet cnt;
        QString error;
        if(m_conn && m_conn->query(QStringLiteral("SELECT COUNT(*) FROM ") + qualified + whereSql,
                                   &cnt, &error)) {
            if(!cnt.rows.isEmpty())
                m_totalRows = cnt.rows.first().value(0).toLongLong();
        } else if(!m_where.isEmpty()) {
            emit statusMessage(QStringLiteral("filter rejected: %1").arg(error));
            return; /* keep the current grid */
        }
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

    /* No ORDER BY means "any order" — and PostgreSQL uses it: a table
     * bigger than a few blocks is read by synchronized scans that begin
     * wherever another scan already is, and an UPDATE moves a row to the
     * end of the heap, so every Refresh showed a different first row and
     * paging (LIMIT/OFFSET) could repeat or skip rows. Unsorted grids on
     * Postgres therefore order by the primary key (ctid, the physical
     * position, when there is none). MySQL/SQLite already return primary-key
     * / rowid order. A user's own header sort (m_orderBy) always wins. */
    QString defaultOrder;
    if(m_orderBy.isEmpty() && m_conn && m_conn->driverType() == SqlDriverType::Postgres) {
        QStringList cols;
        for(const QString &k : std::as_const(pkeys))
            cols << m_conn->quoteIdent(k);
        defaultOrder = cols.isEmpty() ? QStringLiteral("ctid") : cols.join(QStringLiteral(", "));
    }
    const auto buildSql = [&](const QString &order) {
        QString q = QStringLiteral("SELECT * FROM ") + qualified + whereSql;
        if(!order.isEmpty())
            q += QStringLiteral(" ORDER BY ") + order;
        if(limited)
            q += QStringLiteral(" LIMIT %1 OFFSET %2").arg(rc).arg(offset);
        return q;
    };

    QStringList header;
    QVector<QStringList> rows;
    {
        DbResultSet rs;
        QString error;
        bool ok = m_conn && m_conn->query(buildSql(m_orderBy.isEmpty() ? defaultOrder : m_orderBy),
                                          &rs, &error);
        /* a relation with no ctid (a view) refuses the default order — fall
         * back to the plain query rather than fail to open it */
        if(!ok && m_conn && m_orderBy.isEmpty() && !defaultOrder.isEmpty())
            ok = m_conn->query(buildSql(QString()), &rs, &error);
        if(!ok) {
            emit statusMessage(QStringLiteral("query failed: %1").arg(error));
            return;
        }
        header = rs.headers;
        rows = rs.rows;
    }
    m_model->setGrid(header, rows);
    QVector<bool> blobCols;
    for(const ColumnInfo &ci : std::as_const(m_colInfo))
        blobCols << ci.blob;
    m_model->setBlobColumns(blobCols);
    m_grid->resizeColumnsToContents();
    syncFormColumns();
    if(m_checkHeader)
        m_checkHeader->clearChecks();
    m_valid = true;

    /* keep the header's sort arrow in sync after the model reset */
    m_grid->horizontalHeader()->setSortIndicator(m_sortColumn, m_sortDesc ? Qt::DescendingOrder
                                                                          : Qt::AscendingOrder);

    const long long from = rows.isEmpty() ? 0 : offset + 1;
    const long long to = offset + rows.size();
    if(m_nextBtn)
        m_nextBtn->setEnabled(limited && to < m_totalRows);

    m_label->setText(QStringLiteral("%1.%2  —  rows %3–%4 of %5%6%7")
                         .arg(m_db, m_table)
                         .arg(from)
                         .arg(to)
                         .arg(m_totalRows)
                         .arg(m_where.isEmpty() ? QString() : QStringLiteral("   [filtered]"))
                         .arg(m_hasPrimary ? QString() : QStringLiteral("   ⚠ no primary key")));
    emit statusMessage(QStringLiteral("%1.%2 — rows %3–%4 of %5")
                           .arg(m_db, m_table)
                           .arg(from)
                           .arg(to)
                           .arg(m_totalRows));

    refreshTextViewIfShown();
}

/* "Unsort" — upstream ID_UNSORT: drop the header-click ORDER BY and
 * re-query (back to the driver's natural/primary-key order) */
void TableDataView::unsort()
{
    if(!m_valid || (m_orderBy.isEmpty() && m_sortColumn < 0))
        return;
    if(!discardStagedEdits(QStringLiteral("Re-sort")))
        return;
    m_orderBy.clear();
    m_sortColumn = -1;
    m_sortDesc = false;
    m_grid->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    reload();
}

/* Filter ▸ quick items — upstream's ID_FILTER_FIELD_* handlers: the clause
 * from the menu item becomes the WHERE, same pipeline as Custom Filter's OK */
void TableDataView::quickFilter(const QString &where)
{
    if(!m_valid || where.isEmpty())
        return;
    if(!discardStagedEdits(QStringLiteral("Re-query")))
        return;
    applyFilterWhere(where);
}

void TableDataView::refresh()
{
    /* same guard as the sort / re-query paths: reloading rebuilds the grid,
     * which threw away unsaved staged edits without a word */
    if(m_valid && discardStagedEdits(QStringLiteral("Refresh")))
        reload();
}
