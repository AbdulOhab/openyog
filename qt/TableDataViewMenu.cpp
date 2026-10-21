/* TableDataView, the grid's context menu: copy, export, and jumping along a foreign key to the
 * referenced row. The class is declared in TableDataView.h; TableDataView.cpp holds the core. */
#include "TableDataView.h"
#include <QClipboard>
#include "TableDataModel.h"
#include "ExportDialog.h"
#include "ResultExport.h"
#include "db/IDbConnection.h"

#include <QApplication>
#include <QDialog>
#include <QMenu>

#include <algorithm>
#include <functional>

void TableDataView::showContextMenu(const QPoint &pos, const QModelIndex &cell)
{
    if(!m_valid)
        return;
    QMenu *menu = buildContextMenu(cell.isValid() ? cell : m_grid->indexAt(pos));
    menu->exec(m_grid->viewport()->mapToGlobal(pos));
    menu->deleteLater();
}

QMenu *TableDataView::buildContextMenu(const QModelIndex &at)
{
    /* upstream CustomGrid moves the active cell on right-click, so the
     * Set-To/filter/copy items all target the cell under the cursor; a
     * click outside the data (or the keyboard menu key) keeps the current
     * cell — DataView::OnContextMenu's (-1,-1) fallback */
    QModelIndex cell = at;
    if(!cell.isValid())
        cell = m_grid->currentIndex();
    if(cell.isValid())
        m_grid->setCurrentIndex(cell);

    const int row = cell.isValid() ? cell.row() : -1;
    const int col = cell.isValid() ? cell.column() : -1;
    const bool cellOk = row >= 0 && row < m_model->rowCount() && col >= 0 && col < m_columns.size();
    const bool del = cellOk && m_model->rowState(row) == TableDataModel::Deleted;
    const ColumnInfo ci = m_colInfo.value(col); /* defaults when !cellOk */
    const int ops = m_model->pendingOps();
    const int nchecked = checkedRows().size();

    QMenu *menu = new QMenu(this);

    /* -- exactly upstream's list, in .rc order, with DataView's enable rules */
    menu->addAction(QStringLiteral("Insert New Row"), this, &TableDataView::addRow);
    QAction *save =
        menu->addAction(QStringLiteral("Save Changes"), this, &TableDataView::applyPendingEdits);
    QAction *delrows = menu->addAction(QStringLiteral("Delete Selected Row(s)"), this,
                                       &TableDataView::deleteSelectedRow);
    QAction *cancel =
        menu->addAction(QStringLiteral("Cancel Changes"), this, &TableDataView::revertPendingEdits);
    save->setEnabled(ops > 0);
    delrows->setEnabled(m_model->rowCount() > 0 && (nchecked > 0 || cell.isValid()));
    cancel->setEnabled(ops > 0);

    menu->addSeparator();

    QAction *setEmpty =
        menu->addAction(QStringLiteral("Set To Empty String"), this, &TableDataView::setCellEmpty);
    QAction *setNull =
        menu->addAction(QStringLiteral("Set To NULL"), this, &TableDataView::setCellNull);
    QAction *setDef =
        menu->addAction(QStringLiteral("Set To Default"), this, &TableDataView::setCellDefault);
    setEmpty->setEnabled(cellOk && !del);
    /* upstream keeps NULL usable on a non-nullable TIMESTAMP column: writing
     * NULL there makes MySQL store the CURRENT_TIMESTAMP instead */
    setNull->setEnabled(
        cellOk && !del &&
        (ci.nullable || ci.type.compare(QLatin1String("timestamp"), Qt::CaseInsensitive) == 0));
    setDef->setEnabled(cellOk && !del && ci.hasDefault);

    menu->addSeparator();

    QAction *unsort = menu->addAction(QStringLiteral("Unsort"), this, &TableDataView::unsort);
    unsort->setEnabled(m_sortColumn >= 0 || !m_orderBy.isEmpty());

    menu->addSeparator();

    /* Filter ▸ — upstream DataView::SetFilterMenu: every item's LABEL is the
     * clause it will run, built from the cell under the cursor. Upstream's
     * own label rule truncates the value at 20 chars + "..."; the clause
     * that actually executes keeps the full, driver-escaped value (upstream
     * filters on the truncated text too — a bug not worth replicating). A
     * NULL cell turns =/<> into IS [NOT] NULL and disables the other five;
     * binary-typed columns disable all seven (upstream's IsDataBinary). */
    QMenu *filter = menu->addMenu(QStringLiteral("Filter"));
    const QString qcol = m_conn ? m_conn->quoteIdent(m_columns.value(col))
                                : QStringLiteral("`%1`").arg(m_columns.value(col));
    const QString raw = cellOk ? m_model->cur(row, col) : QString();
    const bool nullCell = raw == QStringLiteral("NULL");
    const QString shown = nullCell            ? QStringLiteral("NULL")
                          : raw.length() > 20 ? raw.left(20) + QStringLiteral("...")
                                              : raw;
    const QString escv =
        nullCell || !m_conn ? raw : QString::fromUtf8(m_conn->escape(raw.toUtf8()));
    const bool filterable = cellOk && !ci.blob;
    const auto quickItem = [&](const QString &label, const QString &where, bool on) {
        QAction *a = filter->addAction(label, this, [this, where] { quickFilter(where); });
        a->setEnabled(on);
    };
    quickItem(nullCell ? QStringLiteral("%1 IS NULL").arg(qcol)
                       : QStringLiteral("%1 = '%2'").arg(qcol, shown),
              nullCell ? QStringLiteral("%1 IS NULL").arg(qcol)
                       : QStringLiteral("%1 = '%2'").arg(qcol, escv),
              filterable);
    quickItem(nullCell ? QStringLiteral("%1 IS NOT NULL").arg(qcol)
                       : QStringLiteral("%1 <> '%2'").arg(qcol, shown),
              nullCell ? QStringLiteral("%1 IS NOT NULL").arg(qcol)
                       : QStringLiteral("%1 <> '%2'").arg(qcol, escv),
              filterable);
    filter->addSeparator();
    quickItem(QStringLiteral("%1 > '%2'").arg(qcol, shown),
              QStringLiteral("%1 > '%2'").arg(qcol, escv), filterable && !nullCell);
    quickItem(QStringLiteral("%1 < '%2'").arg(qcol, shown),
              QStringLiteral("%1 < '%2'").arg(qcol, escv), filterable && !nullCell);
    filter->addSeparator();
    /* LIKE patterns are built by concatenation, not .arg(): QString::arg
     * has no %% escape, so a '%2%%' format renders two literal percents —
     * upstream's three variants are ends-with / begins-with / contains */
    quickItem(qcol + QStringLiteral(" LIKE '%") + shown + QLatin1Char('\''),
              qcol + QStringLiteral(" LIKE '%") + escv + QLatin1Char('\''),
              filterable && !nullCell);
    quickItem(qcol + QStringLiteral(" LIKE '") + shown + QStringLiteral("%'"),
              qcol + QStringLiteral(" LIKE '") + escv + QStringLiteral("%'"),
              filterable && !nullCell);
    quickItem(qcol + QStringLiteral(" LIKE '%") + shown + QStringLiteral("%'"),
              qcol + QStringLiteral(" LIKE '%") + escv + QStringLiteral("%'"),
              filterable && !nullCell);
    filter->addSeparator();
    filter->addAction(QStringLiteral("Custom Filter…"), this, &TableDataView::openCustomFilter);
    filter->addSeparator();
    QAction *reset =
        filter->addAction(QStringLiteral("Reset Filter"), this, &TableDataView::resetFilter);
    reset->setEnabled(!m_where.isEmpty());

    menu->addSeparator();

    /* the Ctrl+Shift+E hint mirrors upstream's accelerator label; the key
     * itself is already bound window-wide (Tools menu) and re-binding it
     * here would make the shortcut ambiguous */
    menu->addAction(QStringLiteral("Export All Rows Of Table Data/Result As…\tCtrl+Shift+E"), this,
                    &TableDataView::exportRows);

    QMenu *copy = menu->addMenu(QStringLiteral("Copy"));
    QAction *copyCell = copy->addAction(QStringLiteral("Copy Cell Data To Clipboard"), this,
                                        &TableDataView::copyCellData);
    QAction *copyAll = copy->addAction(QStringLiteral("Copy All Rows To Clipboard…"), this,
                                       &TableDataView::copyAllRows);
    QAction *copySel = copy->addAction(QStringLiteral("Copy Selected Row(s) To Clipboard…"), this,
                                       &TableDataView::copySelectedRows);
    copyCell->setEnabled(cellOk);
    copyAll->setEnabled(m_model->rowCount() > 0);
    copySel->setEnabled(nchecked > 0 || cell.isValid());

    QAction *dup = menu->addAction(QStringLiteral("Duplicate Current Row"), this,
                                   &TableDataView::duplicateRow);
    dup->setEnabled(cell.isValid());

    /* FK jump-to-parent: only offered when the cell's column is a foreign
     * key's local column (information_schema / pragma lookup above) */
    const QPair<QString, QString> fk = cellOk ? fkTarget(col) : QPair<QString, QString>();
    if(cellOk && !fk.first.isEmpty()) {
        QAction *jump =
            menu->addAction(QStringLiteral("Jump to Referenced Row (`%1`)…").arg(fk.first), this,
                            [this, row, col, fk] { jumpToReferenced(row, col, fk); });
        jump->setEnabled(!del);
    }

    /* -- this port's own extras (not in upstream's menu; all pre-existing):
     * the cell/hex editor's only menu entry, the typed-VALUES insert, the
     * row-check column's select-all, refresh -- */
    menu->addSeparator();
    QAction *bigEdit = menu->addAction(QStringLiteral("&Edit Cell in Text Editor…"), this,
                                       &TableDataView::editCellInTextEditor);
    bigEdit->setEnabled(cellOk && !del);
    menu->addAction(QStringLiteral("Add Row (with &values…)"), this,
                    &TableDataView::insertRowWithValues);
    menu->addAction(QStringLiteral("&Check All Rows"), this, [this] { checkAllRows(true); });
    QAction *uncheck =
        menu->addAction(QStringLiteral("&Uncheck All Rows"), this, [this] { checkAllRows(false); });
    uncheck->setEnabled(nchecked > 0);
    menu->addSeparator();
    menu->addAction(QStringLiteral("&Refresh"), this, &TableDataView::refresh);

    return menu;
}

/* (parentTable, parentColumn) of the single-column foreign key that has
 * `col` as its local column, or ("", "") when none — Table Data's
 * Jump-to-Referenced-Row needs no other FK detail. MySQL and PostgreSQL
 * both speak information_schema.key_column_usage; SQLite has its pragma. */
QPair<QString, QString> TableDataView::fkTarget(int col) const
{
    QPair<QString, QString> out;
    if(!m_conn || col < 0 || col >= m_columns.size())
        return out;
    const QString column = m_columns.value(col);
    DbResultSet rs;
    QString msg;
    if(m_conn->driverType() == SqlDriverType::Sqlite) {
        if(!m_conn->query(
               QStringLiteral("PRAGMA foreign_key_list(%1)").arg(m_conn->quoteIdent(m_table)), &rs,
               &msg))
            return out;
        /* id, seq, table, from, to — `to` is empty when the parent's key is
         * its PRIMARY KEY; fall back to that below */
        for(const QStringList &r : rs.rows) {
            if(r.size() >= 5 && r.value(3).compare(column, Qt::CaseInsensitive) == 0) {
                out = {r.value(2), r.value(4)};
                break;
            }
        }
    } else {
        /* MySQL: schema = the table's database. PostgreSQL: the schema the
         * connection's search_path resolves to (its "current schema"). */
        const QString schemaTest = m_conn->driverType() == SqlDriverType::Postgres
                                       ? QStringLiteral("current_schema()")
                                       : QStringLiteral("'%1'").arg(m_db);
        if(!m_conn->query(
               QStringLiteral(
                   "SELECT referenced_table_name, referenced_column_name"
                   " FROM information_schema.key_column_usage"
                   " WHERE table_schema = %1 AND table_name = '%2' AND column_name = '%3'"
                   " AND referenced_table_name IS NOT NULL LIMIT 1")
                   .arg(schemaTest, m_table, column),
               &rs, &msg))
            return out;
        if(!rs.rows.isEmpty() && rs.rows.first().size() >= 2)
            out = {rs.rows.first().value(0), rs.rows.first().value(1)};
    }
    if(out.second.isEmpty() && !out.first.isEmpty())
        out.second = QStringLiteral("id"); /* SQLite's implicit parent key */
    return out;
}

/* context menu's jump: reopen this same view on the parent table, the
 * referencing value as the WHERE — the practical "show me the row this
 * points at" (FEATURES.md's FK jump-to-parent, grid form) */
void TableDataView::jumpToReferenced(int row, int col, const QPair<QString, QString> &fk)
{
    if(!m_conn || fk.first.isEmpty())
        return;
    const QString v = m_model->cur(row, col);
    const QString where = v == QStringLiteral("NULL")
                              ? QStringLiteral("%1 IS NULL").arg(m_conn->quoteIdent(fk.second))
                              : QStringLiteral("%1 = '%2'")
                                    .arg(m_conn->quoteIdent(fk.second),
                                         QString::fromUtf8(m_conn->escape(v.toUtf8())));
    const QString db = m_db;
    load(m_conn, db, fk.first);
    applyFilterWhere(where);
}

/* the shared tab-separated clipboard writer behind the toolbar copy button
 * (checks ▸ selection ▸ all) and the context menu's Copy ▸ items (explicit
 * scopes) — upstream IDM_DATATOCLIPBOARD / IDM_SELDATATOCLIPBOARD */
void TableDataView::copyRowsFrom(const QList<int> &rowsIn, bool withHeader)
{
    if(!m_valid)
        return;
    const QStringList cols = m_model->columns();
    QList<int> rows = rowsIn;
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
    emit statusMessage(QStringLiteral("Copied %1 row(s) to the clipboard").arg(rows.size()));
}

/* copy the selected rows (or all, if nothing is selected) as TSV */
void TableDataView::copyRows(bool withHeader)
{
    if(!m_valid)
        return;
    QList<int> rows = checkedRows();
    if(rows.isEmpty()) {
        const auto sel =
            m_grid->selectionModel() ? m_grid->selectionModel()->selectedRows() : QModelIndexList();
        for(const QModelIndex &idx : sel)
            rows << idx.row();
    }
    if(rows.isEmpty())
        for(int r = 0; r < m_model->rowCount(); ++r)
            rows << r;
    copyRowsFrom(rows, withHeader);
}

void TableDataView::copyAllRows()
{
    QList<int> rows;
    for(int r = 0; r < m_model->rowCount(); ++r)
        rows << r;
    copyRowsFrom(rows, false);
}

void TableDataView::copySelectedRows()
{
    QList<int> rows = checkedRows();
    if(rows.isEmpty()) {
        const auto sel =
            m_grid->selectionModel() ? m_grid->selectionModel()->selectedRows() : QModelIndexList();
        for(const QModelIndex &idx : sel)
            rows << idx.row();
    }
    if(rows.isEmpty()) {
        const QModelIndex cur = m_grid->currentIndex();
        if(cur.isValid())
            rows << cur.row();
    }
    copyRowsFrom(rows, false);
}

void TableDataView::copyCellData()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!idx.isValid())
        return;
    QApplication::clipboard()->setText(m_model->cur(idx.row(), idx.column()));
    emit statusMessage(QStringLiteral("Copied cell to clipboard"));
}

/* dump every loaded row to a CSV file (SQLyog's IDM_IMEX_EXPORTDATA, minus
 * the format picker — CSV only for now) */
void TableDataView::exportRows()
{
    if(!m_valid)
        return;
    const QStringList cols = m_model->columns();
    const QList<int> checked = checkedRows();

    ExportDialog dlg(m_table.isEmpty() ? QStringLiteral("table_data") : m_table, m_table,
                     m_model->rowCount(), !checked.isEmpty(), this);
    if(dlg.exec() != QDialog::Accepted || dlg.path().isEmpty())
        return;

    const bool selOnly = dlg.selectionOnly() && !checked.isEmpty();
    QList<int> rows = selOnly ? checked : QList<int>();
    if(rows.isEmpty())
        for(int r = 0; r < m_model->rowCount(); ++r)
            rows << r;

    const auto cell = [&](int r, int c) { return m_model->cur(rows.at(r), c); };
    ResultExport::Options opt = dlg.options();
    /* IDbConnection has no driverType() getter — quoteIdent()'s own output
     * already tells us which dialect it is, without adding new API surface
     * just for this */
    opt.driver = m_conn && m_conn->quoteIdent(QStringLiteral("x")).startsWith(QLatin1Char('"'))
                     ? SqlDriverType::Postgres
                     : SqlDriverType::Mysql;
    QString err;
    if(ResultExport::write(dlg.path(), dlg.format(), cols, cell, rows.size(), cols.size(), opt,
                           &err))
        emit statusMessage(
            QStringLiteral("Exported %1 row(s) → %2").arg(rows.size()).arg(dlg.path()));
    else
        emit statusMessage(QStringLiteral("Export failed: ") + err);
}
