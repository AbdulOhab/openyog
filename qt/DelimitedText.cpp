#include "DelimitedText.h"

namespace {
/* the character <esc><c> stands for: MySQL's FIELDS ESCAPED BY sequences,
 * anything else (separator, quote, the escape itself, a line end) taken
 * literally. \N (NULL in LOAD DATA) keeps its escape: the import grid has no
 * NULL besides the literal text "NULL", and rewriting it to that would
 * silently change a value that really is "\N". */
QString unescaped(QChar esc, QChar c)
{
    switch(c.unicode()) {
        case 'n':
            return QStringLiteral("\n");
        case 'r':
            return QStringLiteral("\r");
        case 't':
            return QStringLiteral("\t");
        case '0':
            return QString(QChar(0));
        case 'b':
            return QStringLiteral("\b");
        case 'Z':
            return QString(QChar(26));
        case 'N':
            return QString(esc) + c;
        default:
            return QString(c);
    }
}
} // namespace

QList<QStringList> parseDelimitedText(const QString &text, QChar sep, QChar quote, QChar esc)
{
    QList<QStringList> rows;
    QStringList cur;
    QString field;
    bool inQuotes = false;
    const bool escapes = !esc.isNull() && esc != quote;
    const int n = text.size();
    for(int i = 0; i < n;) {
        const QChar c = text[i];
        /* escapes apply in or out of quotes, as in LOAD DATA: ResultExport's
         * "Escaped by" mode writes them into unquoted fields */
        if(escapes && c == esc && i + 1 < n) {
            field += unescaped(esc, text[i + 1]);
            i += 2;
            continue;
        }
        if(inQuotes) {
            if(!quote.isNull() && c == quote) {
                if(i + 1 < n && text[i + 1] == quote) {
                    field += quote;
                    i += 2;
                    continue;
                }
                inQuotes = false;
                ++i;
                continue;
            }
            field += c;
            ++i;
            continue;
        }
        if(!quote.isNull() && c == quote) {
            inQuotes = true;
            ++i;
            continue;
        }
        if(c == sep) {
            cur << field;
            field.clear();
            ++i;
            continue;
        }
        if(c == QLatin1Char('\r')) {
            ++i;
            continue;
        }
        if(c == QLatin1Char('\n')) {
            cur << field;
            field.clear();
            rows << cur;
            cur.clear();
            ++i;
            continue;
        }
        field += c;
        ++i;
    }
    if(!field.isEmpty() || !cur.isEmpty()) {
        cur << field;
        rows << cur;
    }
    return rows;
}
