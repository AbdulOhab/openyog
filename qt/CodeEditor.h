/* OpenYog — query editor with the SQLyog-style line-number gutter.
 * Placeholder until the bundled Scintilla (Qt port) replaces it in Phase 3. */
#pragma once

#include <QPlainTextEdit>
#include <QPaintEvent>

class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent = nullptr);

    int lineNumberAreaWidth() const;

    /* editor ops wired to the Edit menu (Scintilla swap still pending) */
    bool findText(const QString &needle, bool caseSensitive, bool backward);
    void gotoLine(int line);                 /* 1-based */
    void toggleLineComment(bool add);        /* prefix/strip "-- " on sel lines */

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();

private:
    void paintLineNumberArea(QPaintEvent *event);

    QWidget *m_lineNumberArea;
    friend class LineNumberArea;
};

class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(CodeEditor *editor) : QWidget(editor), m_editor(editor)
    {
        setObjectName(QStringLiteral("lineNumberArea"));
    }
    QSize sizeHint() const override
    {
        return QSize(m_editor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        m_editor->paintLineNumberArea(event);
    }

private:
    CodeEditor *m_editor;
};
