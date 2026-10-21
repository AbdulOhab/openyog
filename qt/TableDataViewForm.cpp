/* TableDataView, the Form and Text views and the row checkboxes.
 * The class is declared in TableDataView.h; TableDataView.cpp holds the core. */
#include "TableDataView.h"
#include "TableDataModel.h"
#include "FormView.h"

#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QToolButton>

void TableDataView::setViewMode(int mode)
{
    /* leaving the form: keep what was typed, and hand its row to the grid */
    if(m_viewMode == 1 && mode != 1 && m_formView) {
        m_formView->commit();
        const int r = m_formView->currentRow();
        if(r >= 0 && r < m_model->rowCount() && m_model->columnCount() > 0)
            m_grid->setCurrentIndex(m_model->index(r, qMax(0, m_grid->currentIndex().column())));
    }
    m_viewMode = mode;
    if(mode == 2) {
        m_textView->setPlainText(renderTextView());
        m_viewStack->setCurrentWidget(m_textView);
    } else if(mode == 1) {
        syncFormColumns();
        const QModelIndex cur = m_grid->currentIndex();
        m_formView->setCurrentRow(cur.isValid() ? cur.row() : 0);
        m_viewStack->setCurrentWidget(m_formView);
    } else {
        m_viewStack->setCurrentWidget(m_grid);
    }
    if(m_tbGrid)
        m_tbGrid->setChecked(mode == 0);
    if(m_tbForm)
        m_tbForm->setChecked(mode == 1);
    if(m_tbText)
        m_tbText->setChecked(mode == 2);
}

/* column set (name, type, key, nullability) → the form's field list */
void TableDataView::syncFormColumns()
{
    if(!m_formView)
        return;
    QList<FormView::Column> cols;
    for(int i = 0; i < m_colInfo.size(); ++i) {
        FormView::Column c;
        c.name = m_colInfo[i].name;
        c.type = m_colInfo[i].type;
        c.primary = m_pkColumns.contains(i);
        c.nullable = m_colInfo[i].nullable;
        c.autoInc = m_colInfo[i].autoInc;
        c.blob = m_colInfo[i].blob;
        cols << c;
    }
    m_formView->setColumns(cols);
}

void TableDataView::formNewRow()
{
    if(!m_valid)
        return;
    const int r = m_model->stageNewRow();
    m_formView->setCurrentRow(r);
}

/* same as the grid's Duplicate: auto-increment columns stay blank */
void TableDataView::formDuplicateRow(int src)
{
    if(!m_valid || src < 0 || src >= m_model->rowCount())
        return;
    const int r = m_model->stageNewRow();
    for(int c = 0; c < m_columns.size(); ++c)
        if(!m_colInfo.value(c).autoInc)
            m_model->stage(r, c, m_model->cur(src, c));
    m_formView->setCurrentRow(r);
    emit statusMessage(
        QStringLiteral("Row %1 copied to a new staged row — Apply to insert").arg(src + 1));
}

void TableDataView::formDeleteRow(int row)
{
    if(!m_valid || row < 0 || row >= m_model->rowCount())
        return;
    m_model->toggleDeleted(row); /* an Inserted row just disappears */
}

void TableDataView::formTestCommand(const QString &script)
{
    /* several commands can be chained with '|' */
    const QStringList steps = script.split(QLatin1Char('|'));
    if(steps.size() > 1) {
        for(const QString &s : steps)
            formTestCommand(s);
        return;
    }
    const QString &cmd = script;
    if(cmd == QStringLiteral("form")) {
        setViewMode(1);
    } else if(cmd.startsWith(QStringLiteral("form:"))) {
        if(m_viewMode != 1)
            setViewMode(1);
        m_formView->setCurrentRow(cmd.mid(5).toInt());
    } else if(cmd.startsWith(QStringLiteral("formedit:"))) {
        const QStringList p = cmd.mid(9).split(QLatin1Char(':'));
        if(m_viewMode != 1)
            setViewMode(1);
        if(p.size() >= 2)
            m_formView->setFieldForTest(p[0].toInt(), p.mid(1).join(QLatin1Char(':')));
    } else if(cmd == QStringLiteral("formnew")) {
        if(m_viewMode != 1)
            setViewMode(1);
        formNewRow();
    } else if(cmd.startsWith(QStringLiteral("formnull:"))) {
        if(m_viewMode != 1)
            setViewMode(1);
        m_formView->setNullForTest(cmd.mid(9).toInt(), true);
    } else if(cmd.startsWith(QStringLiteral("formblob:"))) {
        /* formblob:<col>:<hex> — the Form-view Load button's payload without
         * the file dialog: stages raw bytes through the same hook */
        const QStringList p = cmd.mid(9).split(QLatin1Char(':'));
        if(p.size() >= 2)
            m_formView->setBlobForTest(p[0].toInt(),
                                       QByteArray::fromHex(p.mid(1).join(QString()).toUtf8()));
    } else if(cmd == QStringLiteral("formapply")) {
        applyPendingEdits();
    } else if(cmd == QStringLiteral("formdel")) {
        if(m_viewMode != 1)
            setViewMode(1);
        formDeleteRow(m_formView->currentRow());
    } else if(cmd.startsWith(QStringLiteral("jump:"))) {
        /* jump:<row>:<col> — the context menu's FK jump, without the click */
        const QStringList p = cmd.mid(5).split(QLatin1Char(':'));
        if(p.size() >= 2) {
            const auto fk = fkTarget(p[1].toInt());
            if(!fk.first.isEmpty())
                jumpToReferenced(p[0].toInt(), p[1].toInt(), fk);
        }
    }
}

QString TableDataView::formFieldForTest(int col) const
{
    return m_formView ? m_formView->fieldTextForTest(col) : QString();
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
