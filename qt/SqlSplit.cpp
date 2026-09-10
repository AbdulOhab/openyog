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
