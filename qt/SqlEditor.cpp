#include "SqlEditor.h"

#include "wyIni.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPalette>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <Qsci/qscilexersql.h>

namespace {
QString uiIniPath()
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    dir.mkpath(".");
    return dir.filePath("OpenYog.ini");
}
} // namespace

namespace {
/* SQL keywords always offered by the completer */
const QStringList kKeywordWords = {
    "SELECT",
    "FROM",
    "WHERE",
    "GROUP BY",
    "ORDER BY",
    "HAVING",
    "LIMIT",
    "OFFSET",
    "INNER JOIN",
    "LEFT JOIN",
    "RIGHT JOIN",
    "JOIN",
    "ON",
    "AS",
    "AND",
    "OR",
    "NOT",
    "IN",
    "IS NULL",
    "IS NOT NULL",
    "LIKE",
    "BETWEEN",
    "EXISTS",
    "UNION",
    "UNION ALL",
    "DISTINCT",
    "INSERT INTO",
    "VALUES",
    "UPDATE",
    "SET",
    "DELETE FROM",
    "CREATE TABLE",
    "ALTER TABLE",
    "DROP TABLE",
    "TRUNCATE TABLE",
    "CREATE VIEW",
    "CREATE INDEX",
    "PRIMARY KEY",
    "FOREIGN KEY",
    "REFERENCES",
    "DEFAULT",
    "AUTO_INCREMENT",
    "CASE",
    "WHEN",
    "THEN",
    "ELSE",
    "END",
    "ASC",
    "DESC",
    "COUNT(",
    "SUM(",
    "AVG(",
    "MIN(",
    "MAX(",
    "COALESCE(",
    "IFNULL(",
    "NOW()",
    "CURRENT_TIMESTAMP",
};
} // namespace

SqlEditor::SqlEditor(QWidget *parent) : QsciScintilla(parent)
{
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setLexer(new QsciLexerSQL(this));

    /* SQLyog-style chrome: number gutter, caret line, brace matching */
    setMarginLineNumbers(0, true);
    setBraceMatching(QsciScintilla::SloppyBraceMatch);
    setCaretLineVisible(true);
    setIndentationsUseTabs(false);
    setTabWidth(4);
    setBackspaceUnindents(true);
    setAutoIndent(true);

    connect(this, &QsciScintilla::userListActivated, this,
            [this](int, const QString &word) { insertCompletion(word); });

    reskin();
}

void SqlEditor::reskin()
{
    /* Scintilla paints itself, so QSS can't reach it — everything derives
     * from the application palette, and a live theme switch lands here via
     * ApplicationPaletteChange */
    const QPalette &pal = palette();
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    const bool dark = base.lightness() < 128;

    setPaper(base); /* -1 = every style */
    setColor(text);
    setCaretForegroundColor(text);
    setCaretLineBackgroundColor(dark ? base.lighter(115) : QColor(0xF5, 0xF9, 0xFD));
    setMarginsBackgroundColor(dark ? base.lighter(108) : QColor(0xF0, 0xF0, 0xF0));
    setMarginsForegroundColor(dark ? QColor(0x8A, 0x8A, 0x8A) : QColor(Qt::gray));
    setMarginsFont(font());
    setMatchedBraceBackgroundColor(dark ? QColor(0x3A, 0x52, 0x78) : QColor(0xC9, 0xE0, 0xF7));
    setMatchedBraceForegroundColor(text);
    setUnmatchedBraceForegroundColor(dark ? QColor(0xE0, 0x7A, 0x7A) : QColor(0xB0, 0x3A, 0x3A));

    if(auto *lx = qobject_cast<QsciLexerSQL *>(lexer())) {
        /* light colors are the ones the old SqlHighlighter used; dark picks
         * the same hues lightened enough to read on the dark base */
        const QColor kw = dark ? QColor(0x7F, 0xB3, 0xE8) : QColor(0x2A, 0x5D, 0x9F);
        const QColor str = dark ? QColor(0xE0, 0x9E, 0x5A) : QColor(0xB3, 0x50, 0x00);
        const QColor com(0x6A, 0x99, 0x55); /* readable on both */
        const QColor num = dark ? QColor(0xC3, 0xA6, 0xFF) : QColor(0x7A, 0x5A, 0xC0);
        QFont kwf = font();
        kwf.setBold(true);

        struct
        {
            int style;
            const QColor *color;
            const QFont *f;
        } defs[] = {
            {QsciLexerSQL::Default, &text, nullptr},
            {QsciLexerSQL::Comment, &com, nullptr},
            {QsciLexerSQL::CommentLine, &com, nullptr},
            {QsciLexerSQL::CommentLineHash, &com, nullptr},
            {QsciLexerSQL::Number, &num, nullptr},
            {QsciLexerSQL::Keyword, &kw, &kwf},
            {QsciLexerSQL::DoubleQuotedString, &str, nullptr},
            {QsciLexerSQL::SingleQuotedString, &str, nullptr},
            {QsciLexerSQL::Operator, &text, nullptr},
            {QsciLexerSQL::Identifier, &text, nullptr},
            {QsciLexerSQL::QuotedIdentifier, &str, nullptr},
        };
        for(const auto &d : defs) {
            lx->setColor(*d.color, d.style);
            lx->setPaper(base, d.style);
            if(d.f)
                lx->setFont(*d.f, d.style);
        }
        /* upstream EditorFont::SetCase (src/EditorFont.cpp): keywords render
         * uppercase however they were typed — SCI_STYLESETCASE is display
         * only, the buffer keeps the typed case. KeywordCase takes the same
         * three values upstream's sqlyog.ini key does. */
        wyString caseStr;
        wyIni::IniGetString("UserInterface", "KeywordCase", "UPPERCASE", &caseStr,
                            uiIniPath().toUtf8());
        const QByteArray kv = QString::fromUtf8(caseStr.GetString()).toLower().toUtf8();
        const int caseVal = kv == "lowercase"   ? SC_CASE_LOWER
                            : kv == "unchanged" ? SC_CASE_MIXED
                                                : SC_CASE_UPPER;
        SendScintilla(SCI_STYLESETCASE, QsciLexerSQL::Keyword, caseVal);
    }
}

/* ---- QPlainTextEdit-compat shims ------------------------------------- */

QString SqlEditor::toPlainText() const
{
    return text();
}

void SqlEditor::setPlainText(const QString &text_)
{
    setText(text_);
}

/* Scintilla positions are UTF-8 byte offsets; everything text-oriented in
 * this app (statementAt, the ";" scanners, SqlFormat) works in QString
 * units — convert via Qsci's (line, index), which are already character
 * units, and sum the line lengths */
int SqlEditor::cursorPosition() const
{
    int line = 0, index = 0;
    getCursorPosition(&line, &index);
    int abs = 0;
    for(int l = 0; l < line; ++l)
        abs += lineTextNoEol(l).length();
    return abs + index;
}

void SqlEditor::getLineIndex(int pos, int *line, int *index) const
{
    const int n = lines();
    int rest = qMax(0, pos);
    for(int l = 0; l < n; ++l) {
        const int len = lineTextNoEol(l).length();
        if(rest <= len) {
            *line = l;
            *index = rest;
            return;
        }
        rest -= len + 1; /* + the '\n' */
    }
    *line = n - 1;
    *index = lineTextNoEol(n - 1).length();
}

void SqlEditor::moveCursorToEnd()
{
    const int last = qMax(0, lines() - 1);
    setCursorPosition(last, lineTextNoEol(last).length());
}

/* ---- completion ------------------------------------------------------- */

void SqlEditor::setCompletions(const QStringList &words)
{
    m_generic = words;
}

void SqlEditor::setSchema(const QStringList &tables, const QStringList &columns)
{
    m_tables = tables;
    m_columns = columns;
}

void SqlEditor::setColumnLookup(std::function<QStringList(const QString &)> lookup)
{
    m_columnLookup = std::move(lookup);
}

/* The tables (with aliases) the statement around the caret reads or writes:
 * whatever follows FROM/JOIN/UPDATE/INTO, plus a comma-separated list after
 * FROM. Only names present in the schema list count — a subquery alias or a
 * typo just isn't a table here. Whole statement, not just the text before
 * the caret: in "SELECT | FROM t" the table comes after. */
QVector<SqlEditor::StmtTable> SqlEditor::tablesInStatement() const
{
    const int pos = cursorPosition();
    const QString doc = toPlainText();
    const int start = doc.lastIndexOf(QLatin1Char(';'), qMax(0, pos - 1)) + 1;
    int end = doc.indexOf(QLatin1Char(';'), pos);
    if(end < 0)
        end = doc.size();
    const QString stmt = doc.mid(start, end - start);

    static const QRegularExpression tok(
        QStringLiteral("`[^`]+`|\"[^\"]+\"|[A-Za-z_][A-Za-z0-9_$]*|[.,()]"));
    QStringList toks;
    auto it = tok.globalMatch(stmt);
    while(it.hasNext())
        toks << it.next().captured(0);

    static const QSet<QString> notAlias = {
        "WHERE",   "JOIN",   "INNER",     "LEFT",         "RIGHT", "FULL",   "CROSS",
        "NATURAL", "OUTER",  "ON",        "USING",        "GROUP", "ORDER",  "HAVING",
        "LIMIT",   "UNION",  "SET",       "VALUES",       "FROM",  "SELECT", "AS",
        "INTO",    "WINDOW", "RETURNING", "STRAIGHT_JOIN"};
    const auto unquote = [](QString t) {
        if(t.size() >= 2 && (t.startsWith(QLatin1Char('`')) || t.startsWith(QLatin1Char('"'))))
            t = t.mid(1, t.size() - 2);
        return t;
    };
    const auto isIdent = [](const QString &t) {
        return !t.isEmpty() && (t[0].isLetter() || t[0] == QLatin1Char('_') ||
                                t[0] == QLatin1Char('`') || t[0] == QLatin1Char('"'));
    };

    QVector<StmtTable> out;
    for(int i = 0; i < toks.size(); ++i) {
        const QString u = toks[i].toUpper();
        if(u != QLatin1String("FROM") && u != QLatin1String("JOIN") &&
           u != QLatin1String("UPDATE") && u != QLatin1String("INTO"))
            continue;
        const bool list = u == QLatin1String("FROM");
        int j = i + 1;
        while(j < toks.size() && isIdent(toks[j])) {
            QString name = unquote(toks[j++]);
            while(j + 1 < toks.size() && toks[j] == QLatin1String(".") && isIdent(toks[j + 1])) {
                name = unquote(toks[j + 1]); /* schema.table → table */
                j += 2;
            }
            QString alias;
            if(j < toks.size() && toks[j].compare(QLatin1String("AS"), Qt::CaseInsensitive) == 0 &&
               j + 1 < toks.size()) {
                alias = unquote(toks[j + 1]);
                j += 2;
            } else if(j < toks.size() && isIdent(toks[j]) &&
                      !notAlias.contains(toks[j].toUpper())) {
                alias = unquote(toks[j++]);
            }
            for(const QString &t : m_tables)
                if(t.compare(name, Qt::CaseInsensitive) == 0) {
                    out.append({t, alias});
                    break;
                }
            if(list && j < toks.size() && toks[j] == QLatin1String(","))
                ++j;
            else
                break;
        }
    }
    return out;
}

/* the word right before a "." that ends the text before the partial
 * identifier — "e" in "SELECT e.na|" — or empty when there is none */
QString SqlEditor::qualifierBeforeCursor() const
{
    const int pos = cursorPosition();
    const QString doc = toPlainText();
    int e = pos;
    while(e > 0 && (doc[e - 1].isLetterOrNumber() || doc[e - 1] == QLatin1Char('_')))
        --e;
    if(e == 0 || doc[e - 1] != QLatin1Char('.'))
        return {};
    int q = e - 1;
    if(q > 0 && (doc[q - 1] == QLatin1Char('`') || doc[q - 1] == QLatin1Char('"')))
        --q;
    int s = q;
    while(s > 0 && (doc[s - 1].isLetterOrNumber() || doc[s - 1] == QLatin1Char('_')))
        --s;
    return doc.mid(s, q - s);
}

/* Columns of the statement's tables in table order, then ordinal order. With
 * a qualifier ("alias." / "table."), only that table's — empty when the
 * qualifier names none of them, so the caller falls back to every column. */
QStringList SqlEditor::columnsOfTables(const QVector<StmtTable> &tables,
                                       const QString &qualifier) const
{
    QStringList out;
    if(!m_columnLookup)
        return out;
    for(const StmtTable &t : tables) {
        if(!qualifier.isEmpty() && qualifier.compare(t.alias, Qt::CaseInsensitive) != 0 &&
           qualifier.compare(t.table, Qt::CaseInsensitive) != 0)
            continue;
        out += m_columnLookup(t.table);
    }
    out.removeDuplicates();
    return out;
}

/* Which identifiers to offer, from the last significant keyword before the
 * cursor on the current statement.  Mirrors SQLyog's AutoCompleteInterface
 * clause tracking (tables after FROM/JOIN, columns after SELECT/WHERE …). */
SqlEditor::ClauseCtx SqlEditor::clauseContextAtCursor() const
{
    const int pos = cursorPosition();
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

    static const QRegularExpression tok(QStringLiteral("[A-Za-z_][A-Za-z0-9_]*|,|\\(|\\)"));
    QStringList toks;
    auto it = tok.globalMatch(head);
    while(it.hasNext())
        toks << it.next().captured(0);

    for(int i = toks.size() - 1; i >= 0; --i) {
        const QString u = toks[i].toUpper();
        if(u == QLatin1String("FROM") || u == QLatin1String("JOIN") || u == QLatin1String("INTO") ||
           u == QLatin1String("UPDATE") || u == QLatin1String("TABLE") ||
           u == QLatin1String("DESCRIBE"))
            return CtxTable;
        if(u == QLatin1String("SELECT") || u == QLatin1String("WHERE") ||
           u == QLatin1String("ON") || u == QLatin1String("SET") || u == QLatin1String("HAVING") ||
           u == QLatin1String("BY") || u == QLatin1String("USING") ||
           u == QLatin1String("VALUES") || u == QLatin1String("RETURNING"))
            return CtxColumn;
        /* AND / OR / NOT / commas / parens are transparent — keep scanning */
    }
    return CtxAll;
}

QStringList SqlEditor::candidatesForContext() const
{
    QStringList list = kKeywordWords;
    switch(clauseContextAtCursor()) {
        case CtxTable:
            list += m_tables.isEmpty() ? m_generic : m_tables;
            break;
        case CtxColumn: {
            /* the statement's own tables' columns lead, in table order —
             * not merged into the alphabetical pool below, which is what
             * buried them among every column of the database. */
            const QVector<StmtTable> tables = tablesInStatement();
            const QString qualifier = qualifierBeforeCursor();
            const QStringList own = columnsOfTables(tables, qualifier);
            if(!qualifier.isEmpty() && !own.isEmpty())
                return own; /* "alias." — that table's columns and nothing else */
            QStringList rest = list;
            rest += m_columns.isEmpty() ? m_generic : m_columns;
            rest.removeDuplicates();
            rest.sort(Qt::CaseInsensitive);
            QStringList ordered = own;
            for(const QString &w : rest)
                if(!own.contains(w))
                    ordered << w;
            return ordered;
        }
        case CtxAll:
        default:
            list += m_generic;
            list += m_tables;
            list += m_columns;
            break;
    }
    list.removeDuplicates();
    list.sort(Qt::CaseInsensitive);
    return list;
}

QString SqlEditor::wordUnderCursor() const
{
    int line = 0, index = 0;
    getCursorPosition(&line, &index);
    const QString t = lineTextNoEol(line);
    int s = index;
    while(s > 0 && (t[s - 1].isLetterOrNumber() || t[s - 1] == QLatin1Char('_')))
        --s;
    return t.mid(s, index - s);
}

QString SqlEditor::lineTextNoEol(int line) const
{
    QString t = text(line);
    while(t.endsWith(QLatin1Char('\n')) || t.endsWith(QLatin1Char('\r')))
        t.chop(1);
    return t;
}

void SqlEditor::popupCompleter(bool force)
{
    const QString prefix = wordUnderCursor();
    if(!force && prefix.length() < 2) {
        m_lastCompletionCount = 0;
        cancelList();
        return;
    }
    QStringList hits;
    const QStringList list = candidatesForContext();
    for(const QString &w : list)
        if(w.startsWith(prefix, Qt::CaseInsensitive))
            hits << w;
    if(hits.isEmpty() ||
       (hits.size() == 1 && hits.first().compare(prefix, Qt::CaseInsensitive) == 0)) {
        m_lastCompletionCount = 0;
        cancelList();
        return;
    }
    getCursorPosition(&m_ctxLine, &m_ctxIndex);
    m_ctxPrefixLen = prefix.length();
    m_lastCompletionCount = hits.size();
    m_lastHits = hits;
    /* keep candidatesForContext()'s order (own-table columns first) instead
     * of Scintilla's default alphabetical re-sort */
    SendScintilla(SCI_AUTOCSETORDER, static_cast<long>(SC_ORDER_CUSTOM));
    showUserList(1, hits);
}

void SqlEditor::triggerCompletion()
{
    popupCompleter(true);
}

int SqlEditor::completionCountForTest() const
{
    return m_lastCompletionCount;
}

void SqlEditor::insertCompletion(const QString &word)
{
    /* the user may have typed more while the list was open — replace whatever
     * partial word sits at the caret now */
    int line = 0, index = 0;
    getCursorPosition(&line, &index);
    if(line != m_ctxLine)
        return; /* stale popup */
    const QString t = lineTextNoEol(line);
    int s = index;
    while(s > 0 && (t[s - 1].isLetterOrNumber() || t[s - 1] == QLatin1Char('_')))
        --s;
    setSelection(line, s, line, index);
    replaceSelectedText(word);
}

/* ---- events ----------------------------------------------------------- */

void SqlEditor::keyPressEvent(QKeyEvent *event)
{
    const bool ctrlSpace =
        event->key() == Qt::Key_Space && (event->modifiers() & Qt::ControlModifier);
    if(ctrlSpace) {
        popupCompleter(true);
        return;
    }

    QsciScintilla::keyPressEvent(event);

    if(event->text().isEmpty() || (event->modifiers() & ~Qt::ShiftModifier)) /* Ctrl/Alt combos */
        return;
    const QChar ch = event->text().at(0);
    const bool wordChar = ch.isLetterOrNumber() || ch == QLatin1Char('_');
    if(isListActive()) {
        /* Scintilla narrows the open list as typing continues; close it on
         * anything that can't be part of an identifier */
        if(!wordChar && ch != QLatin1Char(' '))
            cancelList();
    } else if(wordChar) {
        popupCompleter(false);
    }
}

void SqlEditor::changeEvent(QEvent *event)
{
    if(event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange)
        reskin();
    QsciScintilla::changeEvent(event);
}

/* ---- Edit-menu ops ----------------------------------------------------- */

bool SqlEditor::findText(const QString &needle, bool caseSensitive, bool backward)
{
    if(needle.isEmpty())
        return false;
    int line = 0, index = 0;
    getCursorPosition(&line, &index);
    /* wrap=true covers the old "search, jump to the other end, retry" loop */
    return findFirst(needle, false, caseSensitive, false, true, !backward, line, index);
}

void SqlEditor::copyWithNormalizedWhitespace()
{
    QString t = hasSelectedText() ? selectedText() : toPlainText();
    static const QRegularExpression ws(QStringLiteral("[ \\t]*\\n[ \\t\\n]*"));
    t.replace(ws, QStringLiteral(" "));
    t.replace(QRegularExpression(QStringLiteral("[ \\t]{2,}")), QStringLiteral(" "));
    QGuiApplication::clipboard()->setText(t.trimmed());
}

void SqlEditor::insertFromFile()
{
    const QString path =
        QFileDialog::getOpenFileName(this, QStringLiteral("Insert file contents"), QString(),
                                     QStringLiteral("SQL / text (*.sql *.txt);;All files (*)"));
    if(path.isEmpty())
        return;
    QFile f(path);
    if(f.open(QIODevice::ReadOnly | QIODevice::Text))
        insert(QString::fromUtf8(f.readAll()));
}

void SqlEditor::gotoLine(int line)
{
    const int l = qBound(0, line - 1, qMax(0, lines() - 1));
    setCursorPosition(l, 0);
    ensureLineVisible(l);
    setFocus();
}

void SqlEditor::toggleLineComment(bool add)
{
    int sl = 0, si = 0, el = 0, ei = 0;
    if(hasSelectedText())
        getSelection(&sl, &si, &el, &ei);
    else
        getCursorPosition(&sl, &si), el = sl;

    beginUndoAction();
    for(int l = sl; l <= el; ++l) {
        const QString t = lineTextNoEol(l);
        if(add) {
            insertAt(QStringLiteral("-- "), l, 0);
        } else {
            int i = 0;
            while(i < t.size() && t[i].isSpace())
                ++i;
            if(t.mid(i).startsWith(QStringLiteral("-- ")))
                setSelection(l, i, l, i + 3), removeSelectedText();
            else if(t.mid(i).startsWith(QStringLiteral("--")))
                setSelection(l, i, l, i + 2), removeSelectedText();
        }
    }
    endUndoAction();
}
