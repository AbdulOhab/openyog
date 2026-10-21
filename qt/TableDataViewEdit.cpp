/* TableDataView, editing: staged cell edits, inserts and deletes and the one-transaction Apply,
 * NULL/empty/default, and the BLOB/long-text cell editor. The class is declared in TableDataView.h;
 * TableDataView.cpp holds the core. */
#include "TableDataView.h"
#include "TableDataModel.h"
#include "FormView.h"
#include "ExportDialog.h"
#include "db/IDbConnection.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QFontDatabase>
#include <QMessageBox>
#include <QRegularExpression>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

bool TableDataView::hasStagedEdits() const
{
    return m_model->pendingOps() != 0;
}

bool TableDataView::discardStagedEdits(const QString &action)
{
    if(m_model->pendingOps() == 0)
        return true;
    if(QMessageBox::question(this, QStringLiteral("Staged edits"),
                             QStringLiteral("There are unsaved staged edits. %1 anyway and "
                                            "discard them?")
                                 .arg(action)) != QMessageBox::Yes)
        return false;
    m_model->revertAll();
    return true;
}

void TableDataView::editCell(int row, int col, const QString &value)
{
    /* selftest entry: stage then commit immediately so the DB reflects it */
    m_model->stage(row, col, value);
    applyPendingEdits();
}

void TableDataView::stageCellOnly(int row, int col, const QString &value)
{
    m_model->stage(row, col, value); /* selftest: leave it pending */
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
    if(m_tbApply)
        m_tbApply->setEnabled(ops > 0);
    if(m_tbRevert)
        m_tbRevert->setEnabled(ops > 0);
    QStringList parts;
    if(edits)
        parts << QStringLiteral("%1 edited row(s)").arg(edits);
    if(ins)
        parts << QStringLiteral("%1 new").arg(ins);
    if(del)
        parts << QStringLiteral("%1 to delete").arg(del);
    m_pendingLabel->setText(parts.join(QStringLiteral(", ")));
}

/* ---- Grid / Form / Text view toggle (SQLyog DataView::SwitchView) --------- */

QString TableDataView::quoteValue(const QString &v) const
{
    if(v == QStringLiteral("NULL"))
        return QStringLiteral("NULL");
    if(!m_conn)
        return QLatin1Char('\'') + v + QLatin1Char('\'');
    return QLatin1Char('\'') + QString::fromUtf8(m_conn->escape(v.toUtf8())) + QLatin1Char('\'');
}

QString TableDataView::whereFromOrigRow(int row) const
{
    const auto qi = [&](const QString &ident) {
        return m_conn ? m_conn->quoteIdent(ident) : QStringLiteral("`%1`").arg(ident);
    };
    QStringList conds;
    const auto addCol = [&](int col) {
        const QString v = m_model->orig(row, col);
        if(v == QStringLiteral("NULL"))
            conds << QStringLiteral("%1 IS NULL").arg(qi(m_columns[col]));
        else
            conds << QStringLiteral("%1 = %2").arg(qi(m_columns[col]), quoteValue(v));
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
    if(m_viewMode == 1 && m_formView)
        m_formView->commit(); /* a field still being typed in counts */
    const QList<int> edited = m_model->dirtyRows();
    const QList<int> inserted = m_model->rowsWithState(TableDataModel::Inserted);
    const QList<int> deleted = m_model->rowsWithState(TableDataModel::Deleted);
    if(edited.isEmpty() && inserted.isEmpty() && deleted.isEmpty())
        return;

    QString lastError;
    const auto fail = [&](const QString &what) {
        m_conn->query(QStringLiteral("ROLLBACK"), nullptr, nullptr);
        emit statusMessage(
            QStringLiteral("Apply failed on %1 (rolled back): %2").arg(what, lastError));
    };
    const auto exec = [&](const QString &sql) { return m_conn->query(sql, nullptr, &lastError); };
    const auto qi = [&](const QString &ident) { return m_conn->quoteIdent(ident); };
    const QString qualified = m_conn->qualify(m_db, m_table);
    const bool dmlLimit = m_conn->supportsLimitOnUpdateDelete();

    exec(QStringLiteral("BEGIN"));

    /* 1. deletes (WHERE from the row's original values) */
    for(int r : deleted) {
        const QString q = QStringLiteral("DELETE FROM %1 WHERE %2%3")
                              .arg(qualified, whereFromOrigRow(r),
                                   dmlLimit ? QStringLiteral(" LIMIT 1") : QString());
        if(!exec(q))
            return fail(QStringLiteral("DELETE"));
    }

    /* 2. inserts (only columns the user filled in) */
    for(int r : inserted) {
        QStringList names, vals;
        for(int c = 0; c < m_columns.size(); ++c) {
            const QString v = m_model->cur(r, c);
            const QString ex = m_model->exprAt(r, c);
            if(v.isEmpty() && ex.isEmpty())
                continue;
            names << qi(m_columns[c]);
            vals << (!ex.isEmpty()                 ? ex
                     : v == QStringLiteral("NULL") ? QStringLiteral("NULL")
                                                   : quoteValue(v));
        }
        const QString q = names.isEmpty() ? m_conn->sqlInsertDefaults(m_db, m_table)
                                          : QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)")
                                                .arg(qualified, names.join(QStringLiteral(", ")),
                                                     vals.join(QStringLiteral(", ")));
        if(!exec(q))
            return fail(QStringLiteral("INSERT"));
    }

    /* 3. updates on edited existing rows */
    for(int r : edited) {
        QStringList setParts;
        for(int c = 0; c < m_columns.size(); ++c) {
            if(!m_model->dirty(r, c))
                continue;
            const QString v = m_model->cur(r, c);
            const QString ex = m_model->exprAt(r, c);
            setParts << QStringLiteral("%1 = %2").arg(
                qi(m_columns[c]), !ex.isEmpty()                 ? ex
                                  : v == QStringLiteral("NULL") ? QStringLiteral("NULL")
                                                                : quoteValue(v));
        }
        const QString where = whereFromOrigRow(r);
        if(setParts.isEmpty() || where.isEmpty())
            continue;
        const QString q = QStringLiteral("UPDATE %1 SET %2 WHERE %3%4")
                              .arg(qualified, setParts.join(QStringLiteral(", ")), where,
                                   dmlLimit ? QStringLiteral(" LIMIT 1") : QString());
        if(!exec(q))
            return fail(QStringLiteral("UPDATE"));
    }

    exec(QStringLiteral("COMMIT"));
    emit statusMessage(QStringLiteral("Applied: %1 updated, %2 inserted, %3 deleted")
                           .arg(edited.size())
                           .arg(inserted.size())
                           .arg(deleted.size()));
    reload(); /* refresh — picks up AUTO_INCREMENT / trigger effects */
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
            m_checkHeader->clearChecks(); /* indices may have shifted */
        return;
    }
    const QModelIndex idx = m_grid->currentIndex();
    if(!idx.isValid())
        return;
    m_model->toggleDeleted(idx.row()); /* staged — commit with Apply */
}

void TableDataView::addRow()
{
    if(!m_valid)
        return;
    const int r = m_model->stageNewRow();
    m_grid->setCurrentIndex(m_model->index(r, 0));
    m_grid->edit(m_model->index(r, 0)); /* jump straight into editing */
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
        QStringLiteral("Row %1 copied to a new staged row — Apply to insert").arg(src + 1));
}

void TableDataView::setCellNull()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid())
        return;
    m_model->stage(idx.row(), idx.column(), QStringLiteral("NULL"));
}

/* "Set To Empty String" — upstream IDC_SETEMPTY (SetValue("")): an empty
 * string is NOT a NULL; on Apply it writes '' */
void TableDataView::setCellEmpty()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid())
        return;
    m_model->stage(idx.row(), idx.column(), QString());
}

/* "Set To Default" — upstream IDC_SETDEF (SetValue("(DEFAULT)")): SQLyog
 * shows the literal "(DEFAULT)" in the cell and rewrites it to the DEFAULT
 * keyword on save; the model's raw-expression path does exactly that for
 * both the UPDATE and the INSERT arms of Apply */
void TableDataView::setCellDefault()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid())
        return;
    m_model->stageExpr(idx.row(), idx.column(), QStringLiteral("(DEFAULT)"),
                       QStringLiteral("DEFAULT"));
}

QByteArray TableDataView::fetchCellBytes(int row, int col) const
{
    QByteArray out;
    if(!m_conn || m_columns.isEmpty())
        return out;
    const QString where = whereFromOrigRow(row);
    if(where.isEmpty())
        return out;
    const QString sql =
        QStringLiteral("SELECT %1 FROM %2 WHERE %3 LIMIT 1")
            .arg(m_conn->quoteIdent(m_columns[col]), m_conn->qualify(m_db, m_table), where);
    /* streamed (raw bytes), not query(), so binary/BLOB content survives
     * unmodified — query()'s DbResultSet rows go through QString::fromUtf8 */
    m_conn->streamQuery(sql, nullptr, nullptr,
                        [&](const QVector<QByteArray> &fields, const QVector<bool> &isNull) {
                            if(!isNull.value(0))
                                out = fields.value(0);
                            return false; /* one row is enough */
                        });
    return out;
}

void TableDataView::editCellInTextEditor()
{
    const QModelIndex idx = m_grid->currentIndex();
    if(!m_valid || !idx.isValid())
        return;
    const int row = idx.row(), c = idx.column();
    const QString col = m_model->columns().value(c);
    /* cur(), not idx.data(): a binary column's untouched DisplayRole is the
     * <BLOB> placeholder — the editor wants the actual (mangled) value */
    const QString cur = m_model->cur(row, c);
    const bool isNull = cur == QStringLiteral("NULL");

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Edit `%1` — row %2").arg(col).arg(row + 1));
    dlg.resize(600, 420);

    auto *tabs = new QTabWidget(&dlg);
    auto *edit = new QPlainTextEdit(tabs);
    edit->setPlainText(isNull ? QString() : cur);
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tabs->addTab(edit, QStringLiteral("Text"));

    /* Hex tab: read-only offset dump on top, an editable raw-hex field below.
     * Typing hex there and pressing OK writes the cell as an x'…' literal;
     * Load/Save move the same content to/from a file (the natural workflow
     * for anything bigger than a handful of bytes). */
    auto *hexPage = new QWidget(tabs);
    auto *hexLay = new QVBoxLayout(hexPage);
    auto *hexDump = new QPlainTextEdit(hexPage);
    hexDump->setReadOnly(true);
    hexDump->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    hexDump->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto *hexEdit = new QPlainTextEdit(hexPage);
    hexEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    hexEdit->setPlaceholderText(QStringLiteral("raw hex, e.g. 48656c6c6f — even number of 0-9 a-f "
                                               "(whitespace ignored); OK writes it as x'…'"));
    auto *loadBtn = new QPushButton(QStringLiteral("&Load from File…"), hexPage);
    auto *saveBtn = new QPushButton(QStringLiteral("&Save to File…"), hexPage);
    auto *fileBtns = new QHBoxLayout;
    fileBtns->addWidget(loadBtn);
    fileBtns->addWidget(saveBtn);
    fileBtns->addStretch(1);
    hexLay->addWidget(new QLabel(QStringLiteral("Bytes:"), hexPage));
    hexLay->addWidget(hexDump, 2);
    hexLay->addWidget(new QLabel(QStringLiteral("Edit as hex:"), hexPage));
    hexLay->addWidget(hexEdit, 1);
    hexLay->addLayout(fileBtns);
    tabs->addTab(hexPage, QStringLiteral("Hex"));

    /* classic hex+ASCII offset dump, shared by the lazy tab load and the
     * Load-from-File button */
    const auto renderDump = [&hexDump](const QByteArray &b) {
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
            dump += QStringLiteral("%1  %2 %3\n").arg(off, 8, 16, QLatin1Char('0')).arg(h, a);
        }
        hexDump->setPlainText(b.isEmpty() ? QStringLiteral("(empty / NULL)") : dump);
        hexDump->appendPlainText(QStringLiteral("\n%1 byte(s)").arg(b.size()));
    };

    bool hexLoaded = false;
    const auto loadHex = [&] {
        hexLoaded = true;
        const QByteArray b = m_model->rowState(row) == TableDataModel::Inserted
                                 ? edit->toPlainText().toUtf8()
                                 : fetchCellBytes(row, c);
        renderDump(b);
        hexEdit->setPlainText(QString::fromLatin1(b.toHex()));
    };
    connect(tabs, &QTabWidget::currentChanged, &dlg, [&](int i) {
        if(i != 1 || hexLoaded)
            return;
        loadHex();
    });
    /* a binary column opens on the Hex tab — the Text tab would just show
     * the mangled fromUtf8 bytes the placeholder is hiding */
    if(c < m_colInfo.size() && m_colInfo.at(c).blob)
        tabs->setCurrentIndex(1);

    /* normalize the editable hex field the same way the OK path does —
     * QByteArray::fromHex would silently DROP bad characters, so validate
     * first and refuse rather than write a corrupted file */
    const auto validHex = [&] {
        const QString h =
            hexEdit->toPlainText().remove(QRegularExpression(QStringLiteral("\\s"))).toLower();
        return !h.contains(QRegularExpression(QStringLiteral("[^0-9a-f]"))) && (h.size() % 2) == 0;
    };
    connect(loadBtn, &QPushButton::clicked, &dlg, [&] {
        const QString f = QFileDialog::getOpenFileName(&dlg, QStringLiteral("Load binary content"));
        if(f.isEmpty())
            return;
        QFile in(f);
        if(!in.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(&dlg, QStringLiteral("Load"), in.errorString());
            return;
        }
        const QByteArray b = in.readAll();
        renderDump(b);
        hexEdit->setPlainText(QString::fromLatin1(b.toHex()));
    });
    connect(saveBtn, &QPushButton::clicked, &dlg, [&] {
        if(!validHex()) {
            QMessageBox::warning(&dlg, QStringLiteral("Hex"),
                                 QStringLiteral("Enter an even number of hex digits (0-9, a-f)."));
            return;
        }
        const QString f = QFileDialog::getSaveFileName(&dlg, QStringLiteral("Save binary content"));
        if(f.isEmpty())
            return;
        QFile out(f);
        if(!out.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(&dlg, QStringLiteral("Save"), out.errorString());
            return;
        }
        out.write(QByteArray::fromHex(
            hexEdit->toPlainText().remove(QRegularExpression(QStringLiteral("\\s"))).toLatin1()));
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    auto *nullBtn = buttons->addButton(QStringLiteral("Set &NULL"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    bool toNull = false;
    connect(nullBtn, &QPushButton::clicked, &dlg, [&] {
        toNull = true;
        dlg.accept();
    });

    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(tabs, 1);
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return;
    if(toNull) {
        m_model->stage(row, c, QStringLiteral("NULL"));
        return;
    }
    if(tabs->currentIndex() == 1) { /* Hex tab is authoritative */
        QString h =
            hexEdit->toPlainText().remove(QRegularExpression(QStringLiteral("\\s"))).toLower();
        if(h.contains(QRegularExpression(QStringLiteral("[^0-9a-f]"))) || (h.size() % 2) != 0) {
            QMessageBox::warning(this, QStringLiteral("Hex"),
                                 QStringLiteral("Enter an even number of hex digits (0-9, a-f)."));
            return;
        }
        const SqlDriverType drv = m_conn ? m_conn->driverType() : SqlDriverType::Mysql;
        const QString expr = hexLiteral(drv, h);
        const QString disp = h.size() > 32 ? hexLiteral(drv, h.left(32) + QStringLiteral("…")) +
                                                 QStringLiteral(" (%1 bytes)").arg(h.size() / 2)
                                           : expr;
        m_model->stageExpr(row, c, disp, expr);
        return;
    }
    m_model->stage(row, c, edit->toPlainText());
}

/* empty fields are skipped; nullable empty fields become NULL */
void TableDataView::insertRowWithValues()
{
    if(!m_valid || !m_conn)
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Add Row — %1.%2").arg(m_db, m_table));
    auto *form = new QFormLayout(&dlg);
    struct Field
    {
        QLineEdit *edit;
        const ColumnInfo *col;
    };
    QVector<Field> fields;
    for(const ColumnInfo &c : std::as_const(m_colInfo)) {
        if(c.autoInc)
            continue; /* let the server number it */
        auto *edit = new QLineEdit(&dlg);
        edit->setPlaceholderText(c.nullable ? QStringLiteral("NULL") : QString());
        form->addRow(c.name + (c.nullable ? QString() : QStringLiteral(" *")), edit);
        fields.append({edit, &c});
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
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
            continue; /* not provided */
        names << m_conn->quoteIdent(f.col->name);
        values << quoteValue(v);
    }
    if(names.isEmpty()) {
        addRow(); /* all defaults */
        return;
    }

    const QString q = QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)")
                          .arg(m_conn->qualify(m_db, m_table), names.join(QStringLiteral(", ")),
                               values.join(QStringLiteral(", ")));
    QString error;
    if(!m_conn->query(q, nullptr, &error)) {
        emit statusMessage(QStringLiteral("INSERT failed: ") + error);
        return;
    }
    reload();
    emit statusMessage(QStringLiteral("1 row inserted into %1.%2").arg(m_db, m_table));
}
