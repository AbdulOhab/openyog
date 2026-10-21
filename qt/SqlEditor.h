/* OpenYog — Scintilla-backed SQL query editor. Replaces the
 * QPlainTextEdit-based CodeEditor (plan.md Phase 3): QsciLexerSQL instead of
 * the regex SqlHighlighter, Scintilla's own line-number margin, caret-line
 * highlight and brace matching, plus the app-level ops CodeEditor grew
 * (clause-aware identifier completion, find, goto line, line comments,
 * normalized-whitespace copy, insert-from-file). Colors derive from the
 * application palette and re-derive on a live theme switch
 * (ApplicationPaletteChange), so light/dark/twilight all stay readable.
 *
 * The rest of the app used to talk to a QPlainTextEdit; the shims it needs
 * live here (toPlainText/setPlainText, QString-unit cursor positions) so the
 * call sites stay text-oriented — Scintilla's native positions are UTF-8
 * byte offsets, which would silently corrupt statementAt()/formatting on
 * any non-ASCII query. */
#pragma once

#include "SqlCompletion.h"

#include <Qsci/qsciscintilla.h>
#include <QStringList>

#include <functional>

class SqlEditor : public QsciScintilla
{
    Q_OBJECT
public:
    explicit SqlEditor(QWidget *parent = nullptr);

    /* QPlainTextEdit-compat shims */
    QString toPlainText() const;
    void setPlainText(const QString &text);

    /* caret offset in QString (UTF-16) units — the unit statementAt(),
     * SqlFormat and the ";" scanners all work in */
    int cursorPosition() const;
    /* QString offset → (line, index-in-line) for setSelection() endpoints */
    void getLineIndex(int pos, int *line, int *index) const;
    void moveCursorToEnd();

    /* app-level editor ops wired to the Edit menu / FindBar */
    bool findText(const QString &needle, bool caseSensitive, bool backward);
    void gotoLine(int line);             /* 1-based */
    void toggleLineComment(bool add);    /* prefix/strip "-- " on sel lines */
    void copyWithNormalizedWhitespace(); /* selection → clipboard, ws runs → " " */
    void insertFromFile();               /* pick a file, insert at the cursor */

    /* autocomplete (Ctrl+Space, typing, or "." after a table/alias): what to
     * offer is SqlCompletion's call — the schema it draws on, and the
     * dialect whose keywords, functions and quoting it uses */
    void setCompletionSchema(const SqlCompletion::Schema &schema);
    void setDialect(SqlDriverType dialect);
    /* piecemeal setters for the same schema (selftests) */
    void setCompletions(const QStringList &words); /* untyped fallback words */
    void setSchema(const QStringList &tables, const QStringList &columns);
    void setColumnLookup(std::function<QStringList(const QString &table)> lookup);
    void triggerCompletion();           /* force the popup now */
    int completionCountForTest() const; /* selftest only */
    QStringList completionHitsForTest() const
    {
        return m_lastHits;
    } /* selftest only */

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    QString lineTextNoEol(int line) const;
    void popupCompleter(bool force);
    void insertCompletion(const QString &word);
    void registerCompletionImages();
    void reskin();

    SqlCompletion::Schema m_schema;
    SqlDriverType m_dialect = SqlDriverType::Mysql;
    int m_lastCompletionCount = 0;
    QStringList m_lastHits;
    /* popup context: the partial word being completed (line, index, length) */
    int m_ctxLine = 0, m_ctxIndex = 0, m_ctxPrefixLen = 0;
};
