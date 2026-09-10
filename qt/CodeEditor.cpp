#include "CodeEditor.h"

#include <QAbstractItemView>
#include <QClipboard>
#include <QCompleter>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextDocument>

namespace {
/* SQL keywords always offered by the completer */
const QStringList kKeywordWords = {
    "SELECT","FROM","WHERE","GROUP BY","ORDER BY","HAVING","LIMIT","OFFSET",
    "INNER JOIN","LEFT JOIN","RIGHT JOIN","JOIN","ON","AS","AND","OR","NOT",
    "IN","IS NULL","IS NOT NULL","LIKE","BETWEEN","EXISTS","UNION","UNION ALL",
    "DISTINCT","INSERT INTO","VALUES","UPDATE","SET","DELETE FROM","CREATE TABLE",
    "ALTER TABLE","DROP TABLE","TRUNCATE TABLE","CREATE VIEW","CREATE INDEX",
    "PRIMARY KEY","FOREIGN KEY","REFERENCES","DEFAULT","AUTO_INCREMENT",
    "CASE","WHEN","THEN","ELSE","END","ASC","DESC","COUNT(","SUM(","AVG(",
    "MIN(","MAX(","COALESCE(","IFNULL(","NOW()","CURRENT_TIMESTAMP",
};
} // namespace

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_lineNumberArea = new LineNumberArea(this);

    m_completer = new QCompleter(this);
    m_completer->setModel(new QStringListModel(kKeywordWords, m_completer));
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated),
            this, &CodeEditor::insertCompletion);

    connect(this, &QPlainTextEdit::blockCountChanged,
            this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest,
            this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged,
            this, &CodeEditor::highlightCurrentLine);

    updateLineNumberAreaWidth();
    highlightCurrentLine();
}

void CodeEditor::setCompletions(const QStringList &words)
{
    m_generic = words;
}

void CodeEditor::setSchema(const QStringList &tables, const QStringList &columns)
{
    m_tables = tables;
    m_columns = columns;
}

/* Which identifiers to offer, from the last significant keyword before the
 * cursor on the current statement.  Mirrors SQLyog's AutoCompleteInterface
 * clause tracking (tables after FROM/JOIN, columns after SELECT/WHERE …). */
CodeEditor::ClauseCtx CodeEditor::clauseContextAtCursor() const
{
    const int pos = textCursor().position();
    const QString doc = toPlainText();
    const int stmtStart = doc.lastIndexOf(QLatin1Char(';'), qMax(0, pos - 1)) + 1;
    QString head = doc.mid(stmtStart, pos - stmtStart);

    /* drop the partial identifier currently being typed */
    int e = head.size();
    while(e > 0 && (head[e - 1].isLetterOrNumber() || head[e - 1] == QLatin1Char('_')))
        --e;
    head.truncate(e);

    /* "alias." / "table." → completing a column of that qualifier */
    int k = head.size();
    while(k > 0 && head[k - 1].isSpace())
        --k;
    if(k > 0 && head[k - 1] == QLatin1Char('.'))
        return CtxColumn;

    static const QRegularExpression tok(
        QStringLiteral("[A-Za-z_][A-Za-z0-9_]*|,|\\(|\\)"));
    QStringList toks;
    auto it = tok.globalMatch(head);
    while(it.hasNext())
        toks << it.next().captured(0);

    for(int i = toks.size() - 1; i >= 0; --i) {
        const QString u = toks[i].toUpper();
        if(u == QLatin1String("FROM") || u == QLatin1String("JOIN")
           || u == QLatin1String("INTO") || u == QLatin1String("UPDATE")
           || u == QLatin1String("TABLE") || u == QLatin1String("DESCRIBE"))
            return CtxTable;
        if(u == QLatin1String("SELECT") || u == QLatin1String("WHERE")
           || u == QLatin1String("ON") || u == QLatin1String("SET")
           || u == QLatin1String("HAVING") || u == QLatin1String("BY")
           || u == QLatin1String("USING") || u == QLatin1String("VALUES")
           || u == QLatin1String("RETURNING"))
            return CtxColumn;
        /* AND / OR / NOT / commas / parens are transparent — keep scanning */
    }
    return CtxAll;
}

void CodeEditor::applyModelForContext()
{
    QStringList list = kKeywordWords;
    switch(clauseContextAtCursor()) {
    case CtxTable:
        list += m_tables.isEmpty() ? m_generic : m_tables;
        break;
    case CtxColumn:
        list += m_columns.isEmpty() ? m_generic : m_columns;
        break;
    case CtxAll:
    default:
        list += m_generic;
        list += m_tables;
        list += m_columns;
        break;
    }
    list.removeDuplicates();
    list.sort(Qt::CaseInsensitive);
    qobject_cast<QStringListModel *>(m_completer->model())->setStringList(list);
}

QString CodeEditor::wordUnderCursor() const
{
    QTextCursor c = textCursor();
    c.select(QTextCursor::WordUnderCursor);
    return c.selectedText();
}

void CodeEditor::insertCompletion(const QString &word)
{
    QTextCursor c = textCursor();
    const int extra = word.length() - m_completer->completionPrefix().length();
    c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor,
                   m_completer->completionPrefix().length());
    c.insertText(word);
    setTextCursor(c);
    Q_UNUSED(extra);
}

void CodeEditor::popupCompleter(bool force)
{
    if(m_completer->widget() != this)
        m_completer->setWidget(this);
    const QString prefix = wordUnderCursor();
    if(!force && prefix.length() < 2) {
        m_completer->popup()->hide();
        return;
    }
    applyModelForContext();                 /* clause-aware identifier set */
    m_completer->setCompletionPrefix(prefix);
    m_completer->popup()->setCurrentIndex(
        m_completer->completionModel()->index(0, 0));
    if(m_completer->completionCount() == 0
       || (m_completer->completionCount() == 1
           && m_completer->currentCompletion().compare(
                  prefix, Qt::CaseInsensitive) == 0)) {
        m_completer->popup()->hide();
        return;
    }
    /* cursorRect() is viewport-relative; QCompleter::complete wants
     * widget-relative — shift past the line-number gutter */
    QRect r = cursorRect().translated(lineNumberAreaWidth(), 0);
    r.setWidth(m_completer->popup()->sizeHintForColumn(0)
               + m_completer->popup()->verticalScrollBar()->sizeHint().width());
    m_completer->complete(r);
}

void CodeEditor::triggerCompletion()
{
    popupCompleter(true);
}

int CodeEditor::completionCountForTest() const
{
    return m_completer->completionCount();
}

void CodeEditor::focusInEvent(QFocusEvent *event)
{
    m_completer->setWidget(this);
    QPlainTextEdit::focusInEvent(event);
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    QAbstractItemView *popup = m_completer->popup();
    if(popup->isVisible()) {
        switch(event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
        case Qt::Key_Escape:
        case Qt::Key_Backtab:
            event->ignore();
            return;                 /* let the popup handle it */
        default:
            break;
        }
    }

    const bool ctrlSpace = event->key() == Qt::Key_Space
                           && (event->modifiers() & Qt::ControlModifier);
    if(ctrlSpace) {
        popupCompleter(true);
        return;
    }

    QPlainTextEdit::keyPressEvent(event);

    if(event->text().isEmpty()
       || (event->modifiers() & ~Qt::ShiftModifier))   /* Ctrl/Alt combos */
        return;
    const QChar ch = event->text().at(0);
    if(ch.isLetterOrNumber() || ch == '_')
        popupCompleter(false);
    else if(ch != ' ')                                  /* keep popup while typing */
        popup->hide();
}

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    for(int max = qMax(1, blockCount()); max >= 10; max /= 10)
        ++digits;
    return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if(dy)
        m_lineNumberArea->scroll(0, dy);
    else
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());

    if(rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth();
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height());
}

void CodeEditor::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> selections;
    if(!isReadOnly()) {
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(palette().alternateBase());
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = textCursor();
        sel.cursor.clearSelection();
        selections.append(sel);
    }
    setExtraSelections(selections);
}

void CodeEditor::paintLineNumberArea(QPaintEvent *event)
{
    QPainter painter(m_lineNumberArea);
    painter.fillRect(event->rect(), QColor(0xF0, 0xF0, 0xF0));

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = (int)blockBoundingGeometry(block).translated(contentOffset()).top();
    int bottom = top + (int)blockBoundingRect(block).height();

    while(block.isValid() && top <= event->rect().bottom()) {
        if(block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(Qt::gray);
            painter.drawText(0, top, m_lineNumberArea->width() - 6,
                             fontMetrics().height(), Qt::AlignRight,
                             QString::number(blockNumber + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + (int)blockBoundingRect(block).height();
        ++blockNumber;
    }
}

/* ---- Edit-menu ops (placeholder until Scintilla) --------------------- */

bool CodeEditor::findText(const QString &needle, bool caseSensitive,
                          bool backward)
{
    if(needle.isEmpty())
        return false;
    QTextDocument::FindFlags f;
    if(caseSensitive) f |= QTextDocument::FindCaseSensitively;
    if(backward)      f |= QTextDocument::FindBackward;
    if(find(needle, f))
        return true;
    /* wrap around */
    QTextCursor c = textCursor();
    c.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
    setTextCursor(c);
    return find(needle, f);
}

void CodeEditor::copyWithNormalizedWhitespace()
{
    QString t = textCursor().selectedText();
    if(t.isEmpty())
        t = toPlainText();
    t.replace(QChar(0x2029), QLatin1Char('\n'));   /* QTextCursor para sep */
    static const QRegularExpression ws(QStringLiteral("[ \\t]*\\n[ \\t\\n]*"));
    t.replace(ws, QStringLiteral(" "));
    t.replace(QRegularExpression(QStringLiteral("[ \\t]{2,}")), QStringLiteral(" "));
    QGuiApplication::clipboard()->setText(t.trimmed());
}

void CodeEditor::insertFromFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Insert file contents"), QString(),
        QStringLiteral("SQL / text (*.sql *.txt);;All files (*)"));
    if(path.isEmpty())
        return;
    QFile f(path);
    if(f.open(QIODevice::ReadOnly | QIODevice::Text))
        textCursor().insertText(QString::fromUtf8(f.readAll()));
}

void CodeEditor::gotoLine(int line)
{
    QTextCursor c(document()->findBlockByLineNumber(qMax(0, line - 1)));
    setTextCursor(c);
    centerCursor();
    setFocus();
}

void CodeEditor::toggleLineComment(bool add)
{
    QTextCursor c = textCursor();
    const int selStart = c.selectionStart(), selEnd = c.selectionEnd();
    c.setPosition(selStart);
    const int firstBlock = c.blockNumber();
    c.setPosition(selEnd);
    const int lastBlock = c.blockNumber();

    c.beginEditBlock();
    for(int b = firstBlock; b <= lastBlock; ++b) {
        QTextBlock blk = document()->findBlockByNumber(b);
        QTextCursor lc(blk);
        if(add) {
            lc.insertText(QStringLiteral("-- "));
        } else {
            const QString t = blk.text();
            int i = 0;
            while(i < t.size() && t[i].isSpace()) ++i;
            if(t.mid(i).startsWith(QStringLiteral("-- "))) {
                lc.setPosition(blk.position() + i);
                lc.setPosition(blk.position() + i + 3, QTextCursor::KeepAnchor);
                lc.removeSelectedText();
            } else if(t.mid(i).startsWith(QStringLiteral("--"))) {
                lc.setPosition(blk.position() + i);
                lc.setPosition(blk.position() + i + 2, QTextCursor::KeepAnchor);
                lc.removeSelectedText();
            }
        }
    }
    c.endEditBlock();
}
