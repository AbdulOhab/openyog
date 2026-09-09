#include "CodeEditor.h"

#include <QFontDatabase>
#include <QPainter>
#include <QTextBlock>
#include <QTextDocument>

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_lineNumberArea = new LineNumberArea(this);

    connect(this, &QPlainTextEdit::blockCountChanged,
            this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest,
            this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged,
            this, &CodeEditor::highlightCurrentLine);

    updateLineNumberAreaWidth();
    highlightCurrentLine();
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
