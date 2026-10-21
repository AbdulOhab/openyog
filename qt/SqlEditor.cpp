#include "SqlEditor.h"

#include "Icons.h"
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
    registerCompletionImages();

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
        /* the default style paints the empty area past the text, and with a
         * lexer attached setPaper() above doesn't reach it — a live theme
         * switch left it in the old (white) paper */
        lx->setDefaultPaper(base);
        lx->setDefaultColor(text);
        SendScintilla(SCI_STYLESETBACK, STYLE_DEFAULT, base);
        SendScintilla(SCI_STYLESETFORE, STYLE_DEFAULT, text);
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

namespace {
/* popup icon per kind (Scintilla's "word?id" list items); 0 = none */
int imageFor(SqlCompletion::Kind k)
{
    switch(k) {
        case SqlCompletion::Kind::Table:
        case SqlCompletion::Kind::Alias:
            return 1;
        case SqlCompletion::Kind::View:
            return 2;
        case SqlCompletion::Kind::Column:
            return 3;
        case SqlCompletion::Kind::Function:
            return 4;
        case SqlCompletion::Kind::Database:
            return 5;
        case SqlCompletion::Kind::Procedure:
            return 6;
        case SqlCompletion::Kind::Keyword:
            break;
    }
    return 0;
}
} // namespace

void SqlEditor::registerCompletionImages()
{
    const struct
    {
        int id;
        const char *file;
    } images[] = {{1, "table.ico"},    {2, "view.ico"},     {3, "column.ico"},
                  {4, "function.ico"}, {5, "database.ico"}, {6, "process.ico"}};
    for(const auto &i : images) {
        const QPixmap pm = Icons::get(QString::fromLatin1(i.file)).pixmap(16, 16);
        if(!pm.isNull())
            registerImage(i.id, pm);
    }
}

void SqlEditor::setCompletionSchema(const SqlCompletion::Schema &schema)
{
    m_schema = schema;
}

void SqlEditor::setDialect(SqlDriverType dialect)
{
    m_dialect = dialect;
}

void SqlEditor::setCompletions(const QStringList &words)
{
    m_schema.extra = words;
}

void SqlEditor::setSchema(const QStringList &tables, const QStringList &columns)
{
    m_schema.tables = tables;
    m_schema.columns = columns;
}

void SqlEditor::setColumnLookup(std::function<QStringList(const QString &)> lookup)
{
    m_schema.columnsOf = std::move(lookup);
}

QString SqlEditor::lineTextNoEol(int line) const
{
    QString t = text(line);
    while(t.endsWith(QLatin1Char('\n')) || t.endsWith(QLatin1Char('\r')))
        t.chop(1);
    return t;
}

/* force = Ctrl+Space. Typing opens the list once two characters of a name are
 * in, or right after "alias." / "table." — never inside a string or comment —
 * and re-filters it on every further character (Scintilla's own narrowing
 * only knows prefixes, the ranking here also matches inside names). */
void SqlEditor::popupCompleter(bool force)
{
    const QString doc = toPlainText();
    const int pos = cursorPosition();
    const QString prefix = SqlCompletion::prefixAt(doc, pos);
    const int before = pos - int(prefix.size()) - 1;
    const bool afterDot = before >= 0 && doc[before] == QLatin1Char('.');
    const auto close = [this] {
        m_lastCompletionCount = 0;
        m_lastHits.clear();
        cancelList();
    };
    if(!force && ((prefix.size() < 2 && !afterDot) ||
                  SqlCompletion::inStringOrComment(doc, pos, m_dialect))) {
        close();
        return;
    }
    QStringList hits, shown;
    for(const SqlCompletion::Item &item : SqlCompletion::complete(doc, pos, m_schema, m_dialect)) {
        hits << item.text;
        const int image = imageFor(item.kind);
        shown << (image ? item.text + QLatin1Char('?') + QString::number(image) : item.text);
    }
    if(hits.isEmpty() ||
       (hits.size() == 1 && hits.first().compare(prefix, Qt::CaseInsensitive) == 0)) {
        close();
        return;
    }
    getCursorPosition(&m_ctxLine, &m_ctxIndex);
    m_ctxPrefixLen = int(prefix.size());
    m_lastCompletionCount = int(hits.size());
    m_lastHits = hits;
    /* keep SqlCompletion's ranking instead of Scintilla's alphabetical
     * re-sort */
    SendScintilla(SCI_AUTOCSETORDER, static_cast<long>(SC_ORDER_CUSTOM));
    showUserList(1, shown);
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
    while(s > 0 && (t[s - 1].isLetterOrNumber() || t[s - 1] == QLatin1Char('_') ||
                    t[s - 1] == QLatin1Char('$')))
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
    const bool listWasOpen = isListActive();

    QsciScintilla::keyPressEvent(event);

    if(event->key() == Qt::Key_Backspace) {
        if(listWasOpen)
            popupCompleter(false); /* re-filter, or close below two characters */
        return;
    }
    if(event->text().isEmpty() ||
       (event->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier))) /* Ctrl/Alt combos */
        return;
    const QChar ch = event->text().at(0);
    if(ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('$') ||
       ch == QLatin1Char('.'))
        popupCompleter(false);
    else if(isListActive())
        cancelList(); /* anything else ends the name being typed */
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
