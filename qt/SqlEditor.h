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

#include <Qsci/qsciscintilla.h>
#include <QStringList>

class SqlEditor : public QsciScintilla
{
    Q_OBJECT
public:
    explicit SqlEditor(QWidget *parent = nullptr);

    /* QPlainTextEdit-compat shims */
    QString toPlainText() const;
    void    setPlainText(const QString &text);

    /* caret offset in QString (UTF-16) units — the unit statementAt(),
     * SqlFormat and the ";" scanners all work in */
    int  cursorPosition() const;
    /* QString offset → (line, index-in-line) for setSelection() endpoints */
    void getLineIndex(int pos, int *line, int *index) const;
    void moveCursorToEnd();

    /* app-level editor ops wired to the Edit menu / FindBar */
    bool findText(const QString &needle, bool caseSensitive, bool backward);
    void gotoLine(int line);                 /* 1-based */
    void toggleLineComment(bool add);        /* prefix/strip "-- " on sel lines */
    void copyWithNormalizedWhitespace();     /* selection → clipboard, ws runs → " " */
    void insertFromFile();                   /* pick a file, insert at the cursor */

    /* keyword + schema identifier list for autocomplete (Ctrl+Space / typing) */
    void setCompletions(const QStringList &words);   /* generic bucket / fallback */
    /* clause-aware split: tables offered after FROM/JOIN/INTO/UPDATE,
     * columns after SELECT/WHERE/ON/SET/HAVING/GROUP BY/ORDER BY (and after
     * a "." qualifier). Empty buckets fall back to the generic list. */
    void setSchema(const QStringList &tables, const QStringList &columns);
    void triggerCompletion();               /* force the popup now */
    int  completionCountForTest() const;    /* selftest only */

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum ClauseCtx { CtxAll, CtxTable, CtxColumn };
    ClauseCtx clauseContextAtCursor() const;
    QStringList candidatesForContext() const;
    QString wordUnderCursor() const;
    QString lineTextNoEol(int line) const;
    void popupCompleter(bool force);
    void insertCompletion(const QString &word);
    void reskin();

    QStringList m_generic;    /* setCompletions() — fallback bucket */
    QStringList m_tables;     /* setSchema() tables */
    QStringList m_columns;    /* setSchema() columns */
    int m_lastCompletionCount = 0;
    /* popup context: the partial word being completed (line, index, length) */
    int m_ctxLine = 0, m_ctxIndex = 0, m_ctxPrefixLen = 0;
};
