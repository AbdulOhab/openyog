/* OpenYog — query editor with the SQLyog-style line-number gutter + SQL
 * highlighter and a Find bar (qt/FindBar). A Scintilla swap is still on the
 * roadmap (Phase 3) but needs a Qt6 QScintilla build. */
#pragma once

#include <QPlainTextEdit>
#include <QPaintEvent>
#include <QStringList>

class QCompleter;

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

    /* keyword + schema identifier list for autocomplete (Ctrl+Space / typing) */
    void setCompletions(const QStringList &words);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;

private slots:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();
    void insertCompletion(const QString &word);

private:
    void paintLineNumberArea(QPaintEvent *event);
    QString wordUnderCursor() const;
    void popupCompleter(bool force);

    QWidget    *m_lineNumberArea;
    QCompleter *m_completer = nullptr;
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
