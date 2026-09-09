#include "SqlFormat.h"

#include <QSet>
#include <QStringList>

namespace {

/* keywords we upper-case */
const QSet<QString> kKeywords = {
    "select","from","where","group","by","having","order","limit","offset",
    "join","inner","left","right","outer","cross","on","using","as","and","or",
    "not","in","is","null","like","between","exists","union","all","distinct",
    "insert","into","values","update","set","delete","create","table","view",
    "alter","drop","add","column","primary","key","foreign","references",
    "index","unique","constraint","default","auto_increment","engine","charset",
    "case","when","then","else","end","asc","desc","with","recursive","returning",
    "count","sum","avg","min","max","true","false","if","ifnull","coalesce",
};

/* major clause keywords → start a fresh line at the base indent */
const QSet<QString> kNewlineBefore = {
    "SELECT","FROM","WHERE","GROUP","HAVING","ORDER","LIMIT","UNION","VALUES",
    "SET","JOIN","LEFT","RIGHT","INNER","OUTER","CROSS","ON","RETURNING",
};

struct Tok { QString text; bool word; };

QList<Tok> lex(const QString &s)
{
    QList<Tok> out;
    int i = 0;
    const int n = s.size();
    while(i < n) {
        const QChar c = s[i];
        if(c.isSpace()) { ++i; continue; }

        /* comments — kept verbatim as their own token */
        if(c == '-' && i + 1 < n && s[i + 1] == '-') {
            int j = s.indexOf('\n', i);
            if(j < 0) j = n;
            out.push_back({ s.mid(i, j - i), false });
            i = j;
            continue;
        }
        if(c == '#') {
            int j = s.indexOf('\n', i);
            if(j < 0) j = n;
            out.push_back({ s.mid(i, j - i), false });
            i = j;
            continue;
        }
        if(c == '/' && i + 1 < n && s[i + 1] == '*') {
            int j = s.indexOf(QStringLiteral("*/"), i + 2);
            j = (j < 0) ? n : j + 2;
            out.push_back({ s.mid(i, j - i), false });
            i = j;
            continue;
        }
        /* quoted string / identifier — verbatim, honouring '' and \' */
        if(c == '\'' || c == '"' || c == '`') {
            const QChar q = c;
            int j = i + 1;
            while(j < n) {
                if(s[j] == '\\' && q != '`') { j += 2; continue; }
                if(s[j] == q) {
                    if(j + 1 < n && s[j + 1] == q) { j += 2; continue; }
                    ++j;
                    break;
                }
                ++j;
            }
            out.push_back({ s.mid(i, j - i), false });
            i = j;
            continue;
        }
        /* word */
        if(c.isLetterOrNumber() || c == '_' || c == '$' || c == '.') {
            int j = i;
            while(j < n && (s[j].isLetterOrNumber() || s[j] == '_'
                            || s[j] == '$' || s[j] == '.'))
                ++j;
            out.push_back({ s.mid(i, j - i), true });
            i = j;
            continue;
        }
        /* punctuation — single char */
        out.push_back({ QString(c), false });
        ++i;
    }
    return out;
}

QString formatOne(const QList<Tok> &toks)
{
    if(toks.isEmpty())
        return {};

    QString out;
    int depth = 0;                 /* paren depth */
    bool selectList = false;       /* between SELECT and FROM at depth 0 */
    auto indent = [&](int extra = 0) {
        return QStringLiteral("\n") + QString((depth + extra) * 2, ' ');
    };
    auto endsWith = [&](QChar ch) {
        for(int k = out.size() - 1; k >= 0; --k) {
            if(out[k] == '\n' || out[k] == ' ') continue;
            return out[k] == ch;
        }
        return false;
    };

    for(int t = 0; t < toks.size(); ++t) {
        QString w = toks[t].text;
        const bool isWord = toks[t].word;
        QString up = w.toUpper();

        /* a -- / # / block comment: keep it, then force a line break so it
         * never swallows the tokens that follow */
        if(!isWord && (w.startsWith(QStringLiteral("--"))
                       || w.startsWith(QChar('#'))
                       || w.startsWith(QStringLiteral("/*")))) {
            while(out.endsWith(' ')) out.chop(1);
            if(!out.isEmpty() && !out.endsWith('\n'))
                out += QStringLiteral(" ");
            out += w;
            if(!w.startsWith(QStringLiteral("/*")) || w.contains('\n'))
                out += indent();
            continue;
        }

        if(isWord && kKeywords.contains(w.toLower()))
            w = up;

        if(w == QStringLiteral("(")) {
            out += w;
            ++depth;
            continue;
        }
        if(w == QStringLiteral(")")) {
            depth = qMax(0, depth - 1);
            if(out.endsWith(' ')) out.chop(1);
            out += w;
            continue;
        }
        if(w == QStringLiteral(",")) {
            if(out.endsWith(' ')) out.chop(1);
            out += w;
            out += (selectList && depth == 0) ? indent(1)
                                              : QStringLiteral(" ");
            continue;
        }
        if(w == QStringLiteral(";")) {
            if(out.endsWith(' ')) out.chop(1);
            out += QStringLiteral(";");
            continue;
        }

        if(isWord && depth == 0) {
            if(up == QStringLiteral("SELECT")) selectList = true;
            if(up == QStringLiteral("FROM"))   selectList = false;
        }

        const bool freshLine =
            isWord && depth == 0 && kNewlineBefore.contains(up)
            && !out.isEmpty()
            /* keep "GROUP BY" / "ORDER BY" / "LEFT JOIN" together */
            && !(up == QStringLiteral("BY"))
            && !out.endsWith(QStringLiteral("LEFT "))
            && !out.endsWith(QStringLiteral("RIGHT "))
            && !out.endsWith(QStringLiteral("INNER "))
            && !out.endsWith(QStringLiteral("OUTER "))
            && !out.endsWith(QStringLiteral("CROSS "));

        const bool andOr = isWord && depth == 0
            && (up == QStringLiteral("AND") || up == QStringLiteral("OR"));

        if(freshLine) {
            while(out.endsWith(' ') || out.endsWith('\n')) out.chop(1);
            out += indent();
        } else if(andOr) {
            while(out.endsWith(' ')) out.chop(1);
            out += indent(1);
        } else if(!out.isEmpty() && !out.endsWith(' ') && !out.endsWith('\n')
                  && !endsWith('(') && !w.startsWith('.') && w != QStringLiteral(".")) {
            if(!out.endsWith('.'))
                out += QStringLiteral(" ");
        }

        out += w;
        if(w != QStringLiteral("."))
            out += QStringLiteral(" ");
    }

    while(out.endsWith(' ') || out.endsWith('\n')) out.chop(1);
    return out;
}

} // namespace

QString SqlFormat::pretty(const QString &sql)
{
    /* one lex pass; split the token list on top-level ';' */
    const QList<Tok> toks = lex(sql);
    QList<QList<Tok>> stmts;
    QList<Tok> cur;
    for(const Tok &tk : toks) {
        if(!tk.word && tk.text == QStringLiteral(";")) {
            if(!cur.isEmpty())
                stmts << cur;
            cur.clear();
        } else {
            cur << tk;
        }
    }
    if(!cur.isEmpty())
        stmts << cur;

    QStringList done;
    for(const QList<Tok> &s : std::as_const(stmts)) {
        const QString f = formatOne(s);
        if(!f.isEmpty())
            done << f + QStringLiteral(";");
    }
    return done.join(QStringLiteral("\n\n"));
}
