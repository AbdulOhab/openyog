#include "SqlSplit.h"

#include <QList>
#include <QRegularExpression>

namespace {
/* one statement's span in the script: [start, end) is its text, its
 * terminator (if it has one) runs on to delimEnd */
struct Segment
{
    int start = 0;
    int end = 0;
    int delimEnd = 0;
    bool hasCode = false; /* anything besides whitespace and comments */
};

bool isIdentChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
}

/* index just past the quoted text opened by sql[i]: a doubled quote stays
 * inside, and so does a backslash-escaped character where the dialect has
 * those; an unterminated one runs to the end */
int skipQuoted(const QString &sql, int i, bool backslashEscapes)
{
    const QChar q = sql[i];
    const int n = sql.size();
    for(++i; i < n; ++i) {
        if(backslashEscapes && sql[i] == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if(sql[i] == q) {
            if(i + 1 < n && sql[i + 1] == q) {
                ++i;
                continue;
            }
            return i + 1;
        }
    }
    return n;
}

/* the `$tag$` opening a PostgreSQL dollar-quoted string at i, or empty —
 * not when the '$' continues an identifier (a$b$) and not for a $1
 * parameter (a tag can't start with a digit) */
QString dollarTagAt(const QString &sql, int i)
{
    if(i > 0 && isIdentChar(sql[i - 1]))
        return QString();
    static const QRegularExpression re(QStringLiteral("\\$(?:[A-Za-z_][A-Za-z0-9_]*)?\\$"));
    const auto m = re.match(sql, i, QRegularExpression::NormalMatch,
                            QRegularExpression::AnchorAtOffsetMatchOption);
    return m.hasMatch() ? m.captured(0) : QString();
}

QList<Segment> scan(const QString &sql, SqlDriverType driver)
{
    const bool mysql = driver == SqlDriverType::Mysql;
    const bool pg = driver == SqlDriverType::Postgres;
    /* AnchorAtOffsetMatchOption anchors the match at i, so no leading ^ */
    static const QRegularExpression delimiterLine(
        QStringLiteral("[ \\t]*DELIMITER[ \\t]+(\\S+)[ \\t]*(\\r?\\n|$)"),
        QRegularExpression::CaseInsensitiveOption);

    QList<Segment> out;
    QString delim = QStringLiteral(";");
    Segment cur;
    bool atLineStart = true;
    const int n = sql.size();

    const auto close = [&](int end, int delimEnd) {
        cur.end = end;
        cur.delimEnd = delimEnd;
        out << cur;
        cur = Segment();
        cur.start = delimEnd;
    };

    int i = 0;
    while(i < n) {
        /* a "DELIMITER xxx" line (only meaningful at the start of a line,
         * and this loop is never inside a string or comment here) */
        if(atLineStart) {
            const auto m = delimiterLine.match(sql, i, QRegularExpression::NormalMatch,
                                               QRegularExpression::AnchorAtOffsetMatchOption);
            if(m.hasMatch()) {
                close(i, i);
                delim = m.captured(1);
                i = m.capturedEnd(0);
                cur.start = i;
                continue; /* still at a line start */
            }
        }
        atLineStart = false;

        /* the terminator first: with DELIMITER $$ on PostgreSQL, "$$" ends
         * the statement rather than opening a dollar quote */
        if(QStringView(sql).mid(i, delim.size()) == delim) {
            close(i, i + int(delim.size()));
            i += delim.size();
            continue;
        }

        const QChar ch = sql[i];
        const QChar next = i + 1 < n ? sql[i + 1] : QChar();

        /* line comments stop AT the newline, so it still marks a line start */
        const bool dashDash = ch == QLatin1Char('-') && next == QLatin1Char('-') &&
                              (!mysql || i + 2 >= n || sql[i + 2].isSpace());
        if(dashDash || (mysql && ch == QLatin1Char('#'))) {
            const int e = sql.indexOf(QLatin1Char('\n'), i);
            i = e < 0 ? n : e;
            continue;
        }
        if(ch == QLatin1Char('/') && next == QLatin1Char('*')) {
            const int e = sql.indexOf(QStringLiteral("*/"), i + 2);
            i = e < 0 ? n : e + 2;
            continue;
        }

        if(ch == QLatin1Char('\'') || ch == QLatin1Char('"') || ch == QLatin1Char('`')) {
            /* PostgreSQL: backslash escapes only inside E'…' */
            const bool eString = pg && ch == QLatin1Char('\'') && i > 0 &&
                                 sql[i - 1].toUpper() == QLatin1Char('E') &&
                                 (i < 2 || !isIdentChar(sql[i - 2]));
            const bool backslash = ch != QLatin1Char('`') && (mysql || eString);
            i = skipQuoted(sql, i, backslash);
            cur.hasCode = true;
            continue;
        }

        if(pg && ch == QLatin1Char('$')) {
            const QString tag = dollarTagAt(sql, i);
            if(!tag.isEmpty()) {
                const int e = sql.indexOf(tag, i + tag.size());
                i = e < 0 ? n : e + int(tag.size());
                cur.hasCode = true;
                continue;
            }
        }

        if(!ch.isSpace())
            cur.hasCode = true;
        atLineStart = ch == QLatin1Char('\n');
        ++i;
    }
    close(n, n);
    return out;
}

QString textOf(const QString &sql, const Segment &s)
{
    return sql.mid(s.start, s.end - s.start).trimmed();
}
} // namespace

QStringList splitStatements(const QString &sql, SqlDriverType driver)
{
    QStringList out;
    for(const Segment &s : scan(sql, driver))
        if(s.hasCode)
            out << textOf(sql, s);
    return out;
}

QString statementAt(const QString &sql, int pos, SqlDriverType driver)
{
    QString last; /* the nearest statement before pos */
    for(const Segment &s : scan(sql, driver)) {
        if(s.start > pos)
            break; /* pos is on a DELIMITER line, which no segment covers */
        const bool covers = pos >= s.start && pos <= s.delimEnd;
        if(s.hasCode) {
            last = textOf(sql, s);
            if(covers)
                return last;
        } else if(covers) {
            return last; /* caret on blank / comment-only text */
        }
    }
    return last;
}
