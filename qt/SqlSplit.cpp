#include "SqlSplit.h"

#include <QRegularExpression>

QStringList splitStatements(const QString &sql)
{
    QStringList out;
    QString cur;
    QString delim = QStringLiteral(";");
    bool inString = false;
    bool atLineStart = true;

    for(int i = 0; i < sql.size(); ++i) {
        const QChar ch = sql[i];

        /* a "DELIMITER xxx" line (only meaningful at the start of a line and
         * outside a string) */
        if(atLineStart && !inString) {
            /* AnchorAtOffsetMatchOption anchors the match at i, so no leading ^ */
            static const QRegularExpression re(
                QStringLiteral("[ \\t]*DELIMITER[ \\t]+(\\S+)[ \\t]*(\\r?\\n|$)"),
                QRegularExpression::CaseInsensitiveOption);
            const auto m = re.match(sql, i,
                                    QRegularExpression::NormalMatch,
                                    QRegularExpression::AnchorAtOffsetMatchOption);
            if(m.hasMatch()) {
                if(!cur.trimmed().isEmpty())
                    out << cur.trimmed();
                cur.clear();
                delim = m.captured(1);
                i = m.capturedEnd(0) - 1;    /* skip the whole line */
                atLineStart = true;
                continue;
            }
        }
        atLineStart = (ch == QLatin1Char('\n'));

        if(ch == QLatin1Char('\'')) {
            if(inString && i + 1 < sql.size() && sql[i + 1] == QLatin1Char('\'')) {
                cur += QStringLiteral("''");
                ++i;
                continue;
            }
            inString = !inString;
            cur += ch;
            continue;
        }

        if(!inString && QStringView(sql).mid(i, delim.size()) == delim) {
            if(!cur.trimmed().isEmpty())
                out << cur.trimmed();
            cur.clear();
            i += delim.size() - 1;
            continue;
        }
        cur += ch;
    }
    if(!cur.trimmed().isEmpty())
        out << cur.trimmed();
    return out;
}

QString statementAt(const QString &sql, int pos)
{
    QString delim = QStringLiteral(";");
    bool inString = false, atLineStart = true;
    int segStart = 0;
    QString last;

    for(int i = 0; i < sql.size(); ++i) {
        const QChar ch = sql[i];

        if(atLineStart && !inString) {
            static const QRegularExpression re(
                QStringLiteral("[ \\t]*DELIMITER[ \\t]+(\\S+)[ \\t]*(\\r?\\n|$)"),
                QRegularExpression::CaseInsensitiveOption);
            const auto m = re.match(sql, i, QRegularExpression::NormalMatch,
                                    QRegularExpression::AnchorAtOffsetMatchOption);
            if(m.hasMatch()) {
                delim = m.captured(1);
                i = m.capturedEnd(0) - 1;
                segStart = i + 1;
                atLineStart = true;
                continue;
            }
        }
        atLineStart = (ch == QLatin1Char('\n'));

        if(ch == QLatin1Char('\'')) {
            if(inString && i + 1 < sql.size() && sql[i + 1] == QLatin1Char('\'')) {
                ++i;
                continue;
            }
            inString = !inString;
            continue;
        }

        if(!inString && QStringView(sql).mid(i, delim.size()) == delim) {
            const int segEnd = i;
            const QString seg = sql.mid(segStart, segEnd - segStart);
            if(!seg.trimmed().isEmpty())
                last = seg.trimmed();
            if(pos >= segStart && pos <= segEnd + int(delim.size()))
                return seg.trimmed();
            i += delim.size() - 1;
            segStart = i + 1;
        }
    }
    const QString tail = sql.mid(segStart).trimmed();
    if(!tail.isEmpty())
        return tail;
    return last;
}
