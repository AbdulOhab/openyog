#include "SqlCompletion.h"

#include "SqlSplit.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace SqlCompletion {
namespace {

/* ---- vocabulary ----------------------------------------------------------- */

enum Dialect : quint8 { My = 1, Pg = 2, Lite = 4, All = My | Pg | Lite };

/* where a keyword belongs: at the start of a statement, inside one, or both
 * (SELECT opens a statement and a subquery alike) */
enum Place : quint8 { Start = 1, Clause = 2, Both = Start | Clause };

struct Word
{
    const char *text;
    quint8 dialects;
    quint8 place;
};

const Word kKeywords[] = {
    /* statements */
    {"SELECT", All, Both},
    {"INSERT INTO", All, Start},
    {"UPDATE", All, Start},
    {"DELETE FROM", All, Start},
    {"WITH", All, Both},
    {"EXPLAIN", All, Start},
    {"CREATE TABLE", All, Start},
    {"CREATE TEMPORARY TABLE", All, Start},
    {"CREATE VIEW", All, Start},
    {"CREATE OR REPLACE VIEW", My | Pg, Start},
    {"CREATE INDEX", All, Start},
    {"CREATE UNIQUE INDEX", All, Start},
    {"CREATE TRIGGER", All, Start},
    {"CREATE DATABASE", My | Pg, Start},
    {"CREATE SCHEMA", Pg, Start},
    {"CREATE PROCEDURE", My | Pg, Start},
    {"CREATE FUNCTION", My | Pg, Start},
    {"CREATE OR REPLACE FUNCTION", Pg, Start},
    {"CREATE EVENT", My, Start},
    {"CREATE MATERIALIZED VIEW", Pg, Start},
    {"CREATE EXTENSION", Pg, Start},
    {"ALTER TABLE", All, Start},
    {"ALTER DATABASE", My | Pg, Start},
    {"DROP TABLE", All, Start},
    {"DROP VIEW", All, Start},
    {"DROP INDEX", All, Start},
    {"DROP TRIGGER", All, Start},
    {"DROP DATABASE", My | Pg, Start},
    {"DROP SCHEMA", Pg, Start},
    {"DROP PROCEDURE", My | Pg, Start},
    {"DROP FUNCTION", My | Pg, Start},
    {"TRUNCATE TABLE", My | Pg, Start},
    {"BEGIN", All, Start},
    {"START TRANSACTION", My | Pg, Start},
    {"COMMIT", All, Start},
    {"ROLLBACK", All, Start},
    {"SHOW DATABASES", My, Start},
    {"SHOW TABLES", My, Start},
    {"SHOW FULL TABLES", My, Start},
    {"SHOW COLUMNS FROM", My, Start},
    {"SHOW CREATE TABLE", My, Start},
    {"SHOW INDEX FROM", My, Start},
    {"SHOW PROCESSLIST", My, Start},
    {"SHOW VARIABLES", My, Start},
    {"SHOW STATUS", My, Start},
    {"SHOW GRANTS", My, Start},
    {"SHOW WARNINGS", My, Start},
    {"SHOW", Pg, Start},
    {"DESCRIBE", My, Start},
    {"USE", My, Start},
    {"CALL", My | Pg, Start},
    {"SET", All, Both},
    {"GRANT", My | Pg, Start},
    {"REVOKE", My | Pg, Start},
    {"REPLACE INTO", My | Lite, Start},
    {"INSERT IGNORE INTO", My, Start},
    {"INSERT OR REPLACE INTO", Lite, Start},
    {"INSERT OR IGNORE INTO", Lite, Start},
    {"LOCK TABLES", My, Start},
    {"UNLOCK TABLES", My, Start},
    {"PRAGMA", Lite, Start},
    {"VACUUM", Pg | Lite, Start},
    {"ANALYZE", All, Start},
    {"REFRESH MATERIALIZED VIEW", Pg, Start},
    {"ATTACH DATABASE", Lite, Start},
    {"DETACH DATABASE", Lite, Start},
    {"DELIMITER", All, Start},

    /* clauses, operators, constraints */
    {"FROM", All, Clause},
    {"WHERE", All, Clause},
    {"GROUP BY", All, Clause},
    {"ORDER BY", All, Clause},
    {"HAVING", All, Clause},
    {"LIMIT", All, Clause},
    {"OFFSET", All, Clause},
    {"JOIN", All, Clause},
    {"INNER JOIN", All, Clause},
    {"LEFT JOIN", All, Clause},
    {"LEFT OUTER JOIN", All, Clause},
    {"RIGHT JOIN", All, Clause},
    {"CROSS JOIN", All, Clause},
    {"FULL OUTER JOIN", Pg | Lite, Clause},
    {"NATURAL JOIN", All, Clause},
    {"STRAIGHT_JOIN", My, Clause},
    {"LATERAL", Pg, Clause},
    {"ON", All, Clause},
    {"USING", All, Clause},
    {"AS", All, Clause},
    {"AND", All, Clause},
    {"OR", All, Clause},
    {"NOT", All, Clause},
    {"IN", All, Clause},
    {"NOT IN", All, Clause},
    {"IS NULL", All, Clause},
    {"IS NOT NULL", All, Clause},
    {"NULL", All, Clause},
    {"NOT NULL", All, Clause},
    {"LIKE", All, Clause},
    {"NOT LIKE", All, Clause},
    {"ILIKE", Pg, Clause},
    {"SIMILAR TO", Pg, Clause},
    {"REGEXP", My, Clause},
    {"GLOB", Lite, Clause},
    {"BETWEEN", All, Clause},
    {"EXISTS", All, Clause},
    {"NOT EXISTS", All, Clause},
    {"UNION", All, Clause},
    {"UNION ALL", All, Clause},
    {"INTERSECT", All, Clause},
    {"EXCEPT", All, Clause},
    {"DISTINCT", All, Clause},
    {"DISTINCT ON", Pg, Clause},
    {"ALL", All, Clause},
    {"ANY", My | Pg, Clause},
    {"INTO", All, Clause},
    {"VALUES", All, Clause},
    {"CASE", All, Clause},
    {"WHEN", All, Clause},
    {"THEN", All, Clause},
    {"ELSE", All, Clause},
    {"END", All, Clause},
    {"ASC", All, Clause},
    {"DESC", All, Clause},
    {"NULLS FIRST", Pg | Lite, Clause},
    {"NULLS LAST", Pg | Lite, Clause},
    {"TRUE", All, Clause},
    {"FALSE", All, Clause},
    {"DEFAULT", All, Clause},
    {"PRIMARY KEY", All, Clause},
    {"FOREIGN KEY", All, Clause},
    {"REFERENCES", All, Clause},
    {"UNIQUE", All, Clause},
    {"CHECK", All, Clause},
    {"CONSTRAINT", All, Clause},
    {"AUTO_INCREMENT", My, Clause},
    {"AUTOINCREMENT", Lite, Clause},
    {"IF EXISTS", All, Clause},
    {"IF NOT EXISTS", All, Clause},
    {"ADD COLUMN", All, Clause},
    {"DROP COLUMN", All, Clause},
    {"MODIFY COLUMN", My, Clause},
    {"CHANGE COLUMN", My, Clause},
    {"ALTER COLUMN", Pg, Clause},
    {"RENAME TO", All, Clause},
    {"RENAME COLUMN", All, Clause},
    {"ON DUPLICATE KEY UPDATE", My, Clause},
    {"ON CONFLICT", Pg | Lite, Clause},
    {"DO NOTHING", Pg | Lite, Clause},
    {"DO UPDATE SET", Pg | Lite, Clause},
    {"RETURNING", Pg | Lite, Clause},
    {"OVER", All, Clause},
    {"PARTITION BY", All, Clause},
    {"FETCH FIRST", Pg, Clause},
    {"ROWS ONLY", Pg, Clause},
    {"FOR UPDATE", My | Pg, Clause},
    {"CASCADE", My | Pg, Clause},
    {"ENGINE", My, Clause},
    {"CHARSET", My, Clause},
    {"COLLATE", All, Clause},
    {"COMMENT", My, Clause},
    {"UNSIGNED", My, Clause},
    {"WITHOUT ROWID", Lite, Clause},
    {"INTERVAL", My | Pg, Clause},
};

/* column types, offered inside CREATE TABLE t ( … ) */
const Word kTypes[] = {
    {"INT", My | Pg, Clause},     {"INTEGER", All, Clause},
    {"BIGINT", My | Pg, Clause},  {"SMALLINT", My | Pg, Clause},
    {"TINYINT", My, Clause},      {"DECIMAL(", My | Pg, Clause},
    {"NUMERIC", All, Clause},     {"FLOAT", My, Clause},
    {"DOUBLE", My, Clause},       {"DOUBLE PRECISION", Pg, Clause},
    {"REAL", Pg | Lite, Clause},  {"VARCHAR(", My | Pg, Clause},
    {"CHAR(", My | Pg, Clause},   {"TEXT", All, Clause},
    {"MEDIUMTEXT", My, Clause},   {"LONGTEXT", My, Clause},
    {"BLOB", My | Lite, Clause},  {"LONGBLOB", My, Clause},
    {"BYTEA", Pg, Clause},        {"DATE", My | Pg, Clause},
    {"DATETIME", My, Clause},     {"TIMESTAMP", My | Pg, Clause},
    {"TIMESTAMPTZ", Pg, Clause},  {"TIME", My | Pg, Clause},
    {"BOOLEAN", My | Pg, Clause}, {"JSON", My | Pg, Clause},
    {"JSONB", Pg, Clause},        {"UUID", Pg, Clause},
    {"SERIAL", Pg, Clause},       {"BIGSERIAL", Pg, Clause},
    {"ENUM(", My, Clause},
};

/* functions: "NAME(" takes arguments, "NAME()" takes none */
const Word kFunctions[] = {
    /* everywhere */
    {"COUNT(", All, Clause},
    {"SUM(", All, Clause},
    {"AVG(", All, Clause},
    {"MIN(", All, Clause},
    {"MAX(", All, Clause},
    {"COALESCE(", All, Clause},
    {"NULLIF(", All, Clause},
    {"CAST(", All, Clause},
    {"ABS(", All, Clause},
    {"ROUND(", All, Clause},
    {"LOWER(", All, Clause},
    {"UPPER(", All, Clause},
    {"LENGTH(", All, Clause},
    {"SUBSTR(", All, Clause},
    {"REPLACE(", All, Clause},
    {"TRIM(", All, Clause},
    {"LTRIM(", All, Clause},
    {"RTRIM(", All, Clause},
    {"ROW_NUMBER()", All, Clause},
    {"RANK()", All, Clause},
    {"DENSE_RANK()", All, Clause},
    {"NTILE(", All, Clause},
    {"LAG(", All, Clause},
    {"LEAD(", All, Clause},
    {"FIRST_VALUE(", All, Clause},
    {"LAST_VALUE(", All, Clause},
    {"CURRENT_TIMESTAMP", All, Clause},
    {"CURRENT_DATE", All, Clause},
    /* MySQL + PostgreSQL */
    {"NOW()", My | Pg, Clause},
    {"CONCAT(", My | Pg, Clause},
    {"CONCAT_WS(", My | Pg, Clause},
    {"SUBSTRING(", My | Pg, Clause},
    {"LEFT(", My | Pg, Clause},
    {"RIGHT(", My | Pg, Clause},
    {"LPAD(", My | Pg, Clause},
    {"RPAD(", My | Pg, Clause},
    {"REVERSE(", My | Pg, Clause},
    {"REPEAT(", My | Pg, Clause},
    {"CHAR_LENGTH(", My | Pg, Clause},
    {"POSITION(", My | Pg, Clause},
    {"FLOOR(", My | Pg, Clause},
    {"CEIL(", My | Pg, Clause},
    {"MOD(", My | Pg, Clause},
    {"POWER(", My | Pg, Clause},
    {"SQRT(", My | Pg, Clause},
    {"GREATEST(", My | Pg, Clause},
    {"LEAST(", My | Pg, Clause},
    {"EXTRACT(", My | Pg, Clause},
    {"MD5(", My | Pg, Clause},
    /* MySQL + SQLite */
    {"IFNULL(", My | Lite, Clause},
    {"GROUP_CONCAT(", My | Lite, Clause},
    {"INSTR(", My | Lite, Clause},
    {"HEX(", My | Lite, Clause},
    {"JSON_EXTRACT(", My | Lite, Clause},
    {"JSON_OBJECT(", My | Lite, Clause},
    {"JSON_ARRAY(", My | Lite, Clause},
    /* PostgreSQL + SQLite */
    {"RANDOM()", Pg | Lite, Clause},
    /* MySQL */
    {"CURDATE()", My, Clause},
    {"CURTIME()", My, Clause},
    {"DATE(", My | Lite, Clause},
    {"DATE_FORMAT(", My, Clause},
    {"DATE_ADD(", My, Clause},
    {"DATE_SUB(", My, Clause},
    {"DATEDIFF(", My, Clause},
    {"TIMESTAMPDIFF(", My, Clause},
    {"TIMESTAMPADD(", My, Clause},
    {"YEAR(", My, Clause},
    {"MONTH(", My, Clause},
    {"DAY(", My, Clause},
    {"HOUR(", My, Clause},
    {"MINUTE(", My, Clause},
    {"WEEK(", My, Clause},
    {"DAYOFWEEK(", My, Clause},
    {"LAST_DAY(", My, Clause},
    {"STR_TO_DATE(", My, Clause},
    {"UNIX_TIMESTAMP(", My, Clause},
    {"FROM_UNIXTIME(", My, Clause},
    {"SUBSTRING_INDEX(", My, Clause},
    {"LOCATE(", My, Clause},
    {"FIELD(", My, Clause},
    {"FIND_IN_SET(", My, Clause},
    {"FORMAT(", My | Lite, Clause},
    {"IF(", My, Clause},
    {"ISNULL(", My, Clause},
    {"CONVERT(", My, Clause},
    {"CEILING(", My, Clause},
    {"TRUNCATE(", My, Clause},
    {"RAND()", My, Clause},
    {"UUID()", My, Clause},
    {"SHA1(", My, Clause},
    {"SHA2(", My, Clause},
    {"JSON_UNQUOTE(", My, Clause},
    {"JSON_CONTAINS(", My, Clause},
    {"JSON_SET(", My, Clause},
    {"LAST_INSERT_ID()", My, Clause},
    {"FOUND_ROWS()", My, Clause},
    {"DATABASE()", My, Clause},
    {"USER()", My, Clause},
    {"VERSION()", My, Clause},
    /* PostgreSQL */
    {"DATE_TRUNC(", Pg, Clause},
    {"DATE_PART(", Pg, Clause},
    {"AGE(", Pg, Clause},
    {"TO_CHAR(", Pg, Clause},
    {"TO_DATE(", Pg, Clause},
    {"TO_TIMESTAMP(", Pg, Clause},
    {"TO_NUMBER(", Pg, Clause},
    {"STRING_AGG(", Pg, Clause},
    {"ARRAY_AGG(", Pg, Clause},
    {"SPLIT_PART(", Pg, Clause},
    {"STRPOS(", Pg, Clause},
    {"BTRIM(", Pg, Clause},
    {"INITCAP(", Pg, Clause},
    {"REGEXP_REPLACE(", Pg, Clause},
    {"REGEXP_MATCHES(", Pg, Clause},
    {"GENERATE_SERIES(", Pg, Clause},
    {"UNNEST(", Pg, Clause},
    {"ARRAY_LENGTH(", Pg, Clause},
    {"JSON_BUILD_OBJECT(", Pg, Clause},
    {"JSONB_BUILD_OBJECT(", Pg, Clause},
    {"JSON_AGG(", Pg, Clause},
    {"JSONB_AGG(", Pg, Clause},
    {"TO_JSON(", Pg, Clause},
    {"TO_JSONB(", Pg, Clause},
    {"JSONB_SET(", Pg, Clause},
    {"GEN_RANDOM_UUID()", Pg, Clause},
    {"CURRENT_USER", Pg, Clause},
    {"PG_SIZE_PRETTY(", Pg, Clause},
    {"PG_TOTAL_RELATION_SIZE(", Pg, Clause},
    /* SQLite */
    {"TIME(", Lite, Clause},
    {"DATETIME(", Lite, Clause},
    {"JULIANDAY(", Lite, Clause},
    {"STRFTIME(", Lite, Clause},
    {"UNIXEPOCH(", Lite, Clause},
    {"IIF(", Lite, Clause},
    {"PRINTF(", Lite, Clause},
    {"TYPEOF(", Lite, Clause},
    {"TOTAL(", Lite, Clause},
    {"UNICODE(", Lite, Clause},
    {"CHAR(", Lite, Clause},
    {"QUOTE(", Lite, Clause},
    {"LAST_INSERT_ROWID()", Lite, Clause},
    {"CHANGES()", Lite, Clause},
    {"JSON_GROUP_ARRAY(", Lite, Clause},
    {"JSON_GROUP_OBJECT(", Lite, Clause},
};

/* reserved words that can't be used as a bare identifier somewhere among the
 * three dialects — quoting one is always harmless, missing one is a syntax
 * error */
const QSet<QString> &reservedWords()
{
    static const QSet<QString> s = {
        "ADD",          "ALL",          "ALTER",
        "ANALYZE",      "AND",          "AS",
        "ASC",          "BETWEEN",      "BY",
        "CASE",         "CHECK",        "COLUMN",
        "CONSTRAINT",   "CREATE",       "CROSS",
        "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP",
        "CURRENT_USER", "DATABASE",     "DEFAULT",
        "DELETE",       "DESC",         "DESCRIBE",
        "DISTINCT",     "DROP",         "ELSE",
        "END",          "EXCEPT",       "EXISTS",
        "FALSE",        "FETCH",        "FOR",
        "FOREIGN",      "FROM",         "FULL",
        "GRANT",        "GROUP",        "HAVING",
        "IN",           "INDEX",        "INNER",
        "INSERT",       "INTERSECT",    "INTERVAL",
        "INTO",         "IS",           "JOIN",
        "KEY",          "KEYS",         "LEFT",
        "LIKE",         "LIMIT",        "NATURAL",
        "NOT",          "NULL",         "OFFSET",
        "ON",           "OR",           "ORDER",
        "OUTER",        "PRIMARY",      "RANGE",
        "RANK",         "REFERENCES",   "REPLACE",
        "RIGHT",        "ROW",          "ROWS",
        "SELECT",       "SET",          "SHOW",
        "TABLE",        "THEN",         "TO",
        "TRUE",         "UNION",        "UNIQUE",
        "UPDATE",       "USE",          "USER",
        "USING",        "VALUES",       "WHEN",
        "WHERE",        "WINDOW",       "WITH",
    };
    return s;
}

quint8 dialectBit(SqlDriverType d)
{
    switch(d) {
        case SqlDriverType::Postgres:
            return Pg;
        case SqlDriverType::Sqlite:
            return Lite;
        case SqlDriverType::Mysql:
            break;
        case SqlDriverType::Mongo:
            break; /* not SQL: never gets here */
    }
    return My;
}

/* every single word any keyword phrase is made of: "is this word a
 * keyword", for telling an operand from an operator */
const QSet<QString> &keywordWords()
{
    static const QSet<QString> s = [] {
        QSet<QString> w;
        for(const Word &k : kKeywords)
            for(const QString &part : QString::fromLatin1(k.text).split(QLatin1Char(' ')))
                w.insert(part);
        w.insert(QStringLiteral("IS"));
        w.insert(QStringLiteral("STRAIGHT_JOIN"));
        return w;
    }();
    return s;
}

/* keyword words that name a value, so what follows them is an operator */
bool isValueWord(const QString &u)
{
    return u == QLatin1String("NULL") || u == QLatin1String("TRUE") ||
           u == QLatin1String("FALSE") || u == QLatin1String("END");
}

/* ---- tokens --------------------------------------------------------------- */

bool isIdentStart(QChar c)
{
    return c.isLetter() || c == QLatin1Char('_');
}

bool isIdentChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('$');
}

struct Tok
{
    enum Type { Name, Quoted, Number, String, Punct } type;
    QString text; /* Name/Quoted: the identifier (unquoted); Punct: the character */
};

bool isPunct(const Tok &t, char c)
{
    return t.type == Tok::Punct && t.text.at(0) == QLatin1Char(c);
}

bool isName(const Tok &t)
{
    return t.type == Tok::Name || t.type == Tok::Quoted;
}

bool isWord(const Tok &t, const char *upper)
{
    return t.type == Tok::Name && t.text.compare(QLatin1String(upper), Qt::CaseInsensitive) == 0;
}

enum class EndState { Normal, String, Comment, QuotedIdent };

/* the `$tag$` opening a PostgreSQL dollar quote at i (same rule as SqlSplit) */
QString dollarTagAt(const QString &s, int i)
{
    if(i > 0 && isIdentChar(s[i - 1]))
        return QString();
    static const QRegularExpression re(QStringLiteral("\\$(?:[A-Za-z_][A-Za-z0-9_]*)?\\$"));
    const auto m = re.match(s, i, QRegularExpression::NormalMatch,
                            QRegularExpression::AnchorAtOffsetMatchOption);
    return m.hasMatch() ? m.captured(0) : QString();
}

/* the statement text as tokens, skipping comments; *end says whether the
 * text stops inside a string, comment or quoted identifier */
QList<Tok> tokenize(const QString &s, SqlDriverType d, EndState *end = nullptr)
{
    const bool my = d == SqlDriverType::Mysql;
    const bool pg = d == SqlDriverType::Postgres;
    QList<Tok> out;
    EndState st = EndState::Normal;
    const int n = s.size();
    int i = 0;
    while(i < n) {
        const QChar c = s[i];
        const QChar nx = i + 1 < n ? s[i + 1] : QChar();
        if(c.isSpace()) {
            ++i;
            continue;
        }
        if((c == QLatin1Char('-') && nx == QLatin1Char('-') &&
            (!my || i + 2 >= n || s[i + 2].isSpace())) ||
           (my && c == QLatin1Char('#'))) {
            const int e = s.indexOf(QLatin1Char('\n'), i);
            if(e < 0) {
                st = EndState::Comment;
                break;
            }
            i = e + 1;
            continue;
        }
        if(c == QLatin1Char('/') && nx == QLatin1Char('*')) {
            const int e = s.indexOf(QStringLiteral("*/"), i + 2);
            if(e < 0) {
                st = EndState::Comment;
                break;
            }
            i = e + 2;
            continue;
        }
        if(c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')) {
            const bool ident = c == QLatin1Char('`') || (c == QLatin1Char('"') && !my);
            const bool eString =
                pg && c == QLatin1Char('\'') && !out.isEmpty() && out.last().type == Tok::Name &&
                out.last().text.compare(QLatin1String("E"), Qt::CaseInsensitive) == 0 && i > 0 &&
                !s[i - 1].isSpace();
            const bool backslash = c != QLatin1Char('`') && (my || eString);
            if(eString)
                out.removeLast(); /* E'…' is one literal */
            QString val;
            bool closed = false;
            int j = i + 1;
            while(j < n) {
                if(backslash && s[j] == QLatin1Char('\\') && j + 1 < n) {
                    val += s[j + 1];
                    j += 2;
                    continue;
                }
                if(s[j] == c) {
                    if(j + 1 < n && s[j + 1] == c) {
                        val += c;
                        j += 2;
                        continue;
                    }
                    closed = true;
                    ++j;
                    break;
                }
                val += s[j++];
            }
            if(!closed) {
                st = ident ? EndState::QuotedIdent : EndState::String;
                break;
            }
            out.append({ident ? Tok::Quoted : Tok::String, val});
            i = j;
            continue;
        }
        if(pg && c == QLatin1Char('$')) {
            const QString tag = dollarTagAt(s, i);
            if(!tag.isEmpty()) {
                const int e = s.indexOf(tag, i + tag.size());
                if(e < 0) {
                    st = EndState::String;
                    break;
                }
                out.append({Tok::String, s.mid(i + tag.size(), e - i - tag.size())});
                i = e + int(tag.size());
                continue;
            }
        }
        if(c.isDigit()) {
            int j = i;
            while(j < n && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('.')))
                ++j;
            out.append({Tok::Number, s.mid(i, j - i)});
            i = j;
            continue;
        }
        if(isIdentStart(c)) {
            int j = i;
            while(j < n && isIdentChar(s[j]))
                ++j;
            out.append({Tok::Name, s.mid(i, j - i)});
            i = j;
            continue;
        }
        out.append({Tok::Punct, QString(c)});
        ++i;
    }
    if(end)
        *end = st;
    return out;
}

/* the (possibly dotted) name ending at token j — "t" for db.t — and moves j
 * to the token just before it; empty when token j isn't a name */
QString nameEndingAt(const QList<Tok> &t, int &j)
{
    if(j < 0 || !isName(t[j]))
        return QString();
    const QString name = t[j].text;
    while(j >= 2 && isPunct(t[j - 1], '.') && isName(t[j - 2]))
        j -= 2;
    --j;
    return name;
}

/* ---- the statement's tables ---------------------------------------------- */

struct StmtTable
{
    QString table, alias;
};

QString canonical(const QStringList &names, const QString &n)
{
    for(const QString &x : names)
        if(x.compare(n, Qt::CaseInsensitive) == 0)
            return x;
    return QString();
}

/* tables (with aliases) after FROM / JOIN / UPDATE / INTO, plus FROM's
 * comma list, that the schema knows — a subquery alias or a typo isn't one */
QVector<StmtTable> tablesIn(const QList<Tok> &t, const Schema &schema)
{
    const QStringList known = schema.tables + schema.views;
    QVector<StmtTable> out;
    for(int i = 0; i < t.size(); ++i) {
        const bool from = isWord(t[i], "FROM");
        if(!from && !isWord(t[i], "JOIN") && !isWord(t[i], "UPDATE") && !isWord(t[i], "INTO") &&
           !isWord(t[i], "STRAIGHT_JOIN"))
            continue;
        int j = i + 1;
        while(j < t.size() && isName(t[j])) {
            QString name = t[j++].text;
            while(j + 1 < t.size() && isPunct(t[j], '.') && isName(t[j + 1])) {
                name = t[j + 1].text; /* db.table → table */
                j += 2;
            }
            QString alias;
            if(j + 1 < t.size() && isWord(t[j], "AS") && isName(t[j + 1])) {
                alias = t[j + 1].text;
                j += 2;
            } else if(j < t.size() && isName(t[j]) &&
                      (t[j].type == Tok::Quoted || !keywordWords().contains(t[j].text.toUpper()))) {
                alias = t[j++].text;
            }
            const QString table = canonical(known, name);
            if(!table.isEmpty())
                out.append({table, alias});
            if(from && j < t.size() && isPunct(t[j], ','))
                ++j;
            else
                break;
        }
    }
    return out;
}

/* ---- the clause around the caret ----------------------------------------- */

struct Ctx
{
    enum Type {
        Any,           /* statement start (or nothing recognisable) */
        Table,         /* FROM / JOIN / INTO / UPDATE … */
        AfterTable,    /* FROM t | */
        Column,        /* SELECT / WHERE / ON / SET / BY … */
        AfterOperand,  /* WHERE a | */
        Values,        /* VALUES ( … */
        Database,      /* USE | */
        Procedure,     /* CALL | */
        InsertColumns, /* INSERT INTO t ( | */
        TableColumns,  /* ALTER TABLE t DROP COLUMN | */
        DdlColumns,    /* CREATE TABLE t ( | */
        Qualified,     /* x.| */
        Nothing        /* AS | , LIMIT | — a new alias or a number goes here */
    } type = Any;
    /* InsertColumns / TableColumns: the table; Qualified: the qualifier;
     * otherwise the governing keyword, upper-cased */
    QString name;
};

bool isTableKeyword(const QString &u)
{
    static const QSet<QString> s = {"FROM",     "JOIN",     "INTO",          "UPDATE", "TABLE",
                                    "DESCRIBE", "TRUNCATE", "STRAIGHT_JOIN", "EXPLAIN"};
    return s.contains(u);
}

bool isColumnKeyword(const QString &u)
{
    static const QSet<QString> s = {"SELECT", "WHERE", "ON",   "SET",  "HAVING",    "BY",  "USING",
                                    "WHEN",   "THEN",  "ELSE", "CASE", "RETURNING", "AND", "OR",
                                    "NOT",    "IN",    "LIKE", "IS",   "DISTINCT"};
    return s.contains(u);
}

/* governing keywords that AND/OR/NOT … sit under: those only count as the
 * governing keyword when nothing stronger precedes them */
bool isConnective(const QString &u)
{
    static const QSet<QString> s = {"AND", "OR", "NOT", "IN", "LIKE", "IS", "DISTINCT"};
    return s.contains(u);
}

bool isOperand(const QList<Tok> &t, int i)
{
    const Tok &k = t[i];
    switch(k.type) {
        case Tok::Number:
        case Tok::String:
        case Tok::Quoted:
            return true;
        case Tok::Name: {
            const QString u = k.text.toUpper();
            return !keywordWords().contains(u) || isValueWord(u);
        }
        case Tok::Punct:
            if(isPunct(k, ')'))
                return true;
            /* "SELECT *" / "t.*" — but not "a * b" */
            if(isPunct(k, '*'))
                return i == 0 || isPunct(t[i - 1], ',') || isPunct(t[i - 1], '.') ||
                       isWord(t[i - 1], "SELECT") || isWord(t[i - 1], "DISTINCT");
            return false;
    }
    return false;
}

Ctx analyze(const QList<Tok> &t)
{
    Ctx c;
    const int n = t.size();
    if(n >= 2 && isPunct(t[n - 1], '.') && isName(t[n - 2])) {
        c.type = Ctx::Qualified;
        c.name = t[n - 2].text;
        return c;
    }
    if(n >= 1 && t[n - 1].type == Tok::Name) {
        const QString u = t[n - 1].text.toUpper();
        if(u == QLatin1String("AS") || u == QLatin1String("LIMIT") || u == QLatin1String("OFFSET"))
            return {Ctx::Nothing, {}};
    }

    int governing = -1;
    int depth = 0;
    for(int i = n - 1; i >= 0 && c.type == Ctx::Any; --i) {
        const Tok &k = t[i];
        if(isPunct(k, ')')) {
            ++depth;
            continue;
        }
        if(isPunct(k, '(')) {
            if(depth > 0) {
                --depth;
                continue;
            }
            /* the innermost open paren: an INSERT column list or a CREATE
             * TABLE body; anything else (a call, IN (…), a subquery) is
             * transparent */
            int j = i - 1;
            const QString name = nameEndingAt(t, j);
            if(!name.isEmpty() && j >= 0 && isWord(t[j], "INTO"))
                return {Ctx::InsertColumns, name};
            if(!name.isEmpty() && j >= 2 && isWord(t[j], "EXISTS") && isWord(t[j - 1], "NOT") &&
               isWord(t[j - 2], "IF"))
                j -= 3;
            if(!name.isEmpty() && j >= 0 && isWord(t[j], "TABLE"))
                return {Ctx::DdlColumns, {}};
            continue;
        }
        if(depth > 0 || k.type != Tok::Name)
            continue;
        const QString u = k.text.toUpper();
        if(isTableKeyword(u))
            c.type = Ctx::Table;
        else if(isColumnKeyword(u) && !isConnective(u))
            c.type = Ctx::Column;
        else if(isConnective(u))
            continue; /* keep looking for the clause they belong to */
        else if(u == QLatin1String("VALUES") || u == QLatin1String("VALUE"))
            c.type = Ctx::Values;
        else if(u == QLatin1String("USE"))
            c.type = Ctx::Database;
        else if(u == QLatin1String("CALL"))
            c.type = Ctx::Procedure;
        else
            continue;
        governing = i;
    }

    if(governing >= 0)
        c.name = t[governing].text.toUpper();

    /* ALTER TABLE t DROP / MODIFY / CHANGE / ALTER / RENAME [COLUMN] | → t's
     * columns; ADD COLUMN | names a new one */
    if(c.type == Ctx::Table && governing >= 0 && isWord(t[governing], "TABLE") &&
       governing + 1 < n - 1 && isName(t[governing + 1])) {
        int j = governing + 1;
        QString table = t[j].text;
        while(j + 2 < n && isPunct(t[j + 1], '.') && isName(t[j + 2])) {
            table = t[j + 2].text;
            j += 2;
        }
        const Tok &last = t[n - 1];
        const bool column = isWord(last, "COLUMN");
        if(column && isWord(t[n - 2], "ADD"))
            return {Ctx::Nothing, {}};
        if(j < n - 1 && (column || isWord(last, "DROP") || isWord(last, "MODIFY") ||
                         isWord(last, "CHANGE") || isWord(last, "ALTER") || isWord(last, "RENAME")))
            return {Ctx::TableColumns, table};
    }

    /* right after an operand the next word is an operator or a clause
     * keyword, not another name */
    if(n > 0 && governing >= 0 && governing < n - 1) {
        if(c.type == Ctx::Table && isName(t[n - 1]) &&
           (t[n - 1].type == Tok::Quoted || !keywordWords().contains(t[n - 1].text.toUpper())))
            c = {Ctx::AfterTable, t[governing].text.toUpper()};
        else if(c.type == Ctx::Column && isOperand(t, n - 1))
            c = {Ctx::AfterOperand, t[governing].text.toUpper()};
    }
    return c;
}

/* ---- candidates and ranking ---------------------------------------------- */

struct Cand
{
    QString match;  /* compared with the prefix */
    QString insert; /* what goes into the editor */
    Kind kind;
    int group;  /* lower = more relevant here */
    bool ident; /* identifiers also match by substring / fuzzily */
};

/* 0 prefix, 1 word-boundary prefix, 2 substring, 3 in-order letters; -1 none */
int matchTier(const QString &cand, const QString &prefix, bool ident)
{
    if(prefix.isEmpty() || cand.startsWith(prefix, Qt::CaseInsensitive))
        return 0;
    for(int i = 1; i < cand.size(); ++i) {
        const QChar a = cand[i - 1], b = cand[i];
        const bool boundary = a == QLatin1Char('_') || a == QLatin1Char(' ') ||
                              a == QLatin1Char('.') || (a.isLower() && b.isUpper());
        if(boundary && QStringView(cand).mid(i).startsWith(prefix, Qt::CaseInsensitive))
            return 1;
    }
    if(!ident)
        return -1;
    if(prefix.size() >= 2 && cand.contains(prefix, Qt::CaseInsensitive))
        return 2;
    if(prefix.size() >= 3 && cand.at(0).toLower() == prefix.at(0).toLower()) {
        int k = 0;
        for(int i = 0; i < cand.size() && k < prefix.size(); ++i)
            if(cand[i].toLower() == prefix[k].toLower())
                ++k;
        if(k == prefix.size())
            return 3;
    }
    return -1;
}

class Builder
{
public:
    Builder(SqlDriverType d, bool userQuoted, const QList<Tok> &head)
        : m_d(d), m_bit(dialectBit(d)), m_userQuoted(userQuoted), m_head(head)
    {}

    void idents(QStringList names, Kind k, int group, bool sort = true, const QString &suffix = {})
    {
        if(sort)
            std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
                return a.compare(b, Qt::CaseInsensitive) < 0;
            });
        for(const QString &nm : std::as_const(names))
            if(!nm.isEmpty())
                m_c.append(
                    {nm, (m_userQuoted ? nm : quoteIfNeeded(nm, m_d)) + suffix, k, group, true});
    }

    /* keyword phrases, in the lists' own most-used-first order (WHERE before
     * WHEN, INSERT INTO before INSERT IGNORE INTO). When the text before the
     * caret already ends with a phrase's first words ("GROUP" for GROUP BY)
     * only the rest is offered, and ahead of everything else */
    void words(const Word *list, int count, quint8 place, Kind k, int group)
    {
        for(int i = 0; i < count; ++i) {
            const Word *w = &list[i];
            if(!(w->dialects & m_bit) || !(w->place & place))
                continue;
            const QString text = QString::fromLatin1(w->text);
            /* a JOIN's ON isn't INSERT's ON DUPLICATE KEY / ON CONFLICT */
            const bool insertOnly = m_joinOn && text.startsWith(QLatin1String("ON "));
            const QString rest = insertOnly ? QString() : remainderAfterHead(text);
            if(!rest.isEmpty())
                m_c.append({rest, rest, k, kRemainderGroup, false});
            m_c.append({text, text, k, group, false});
        }
    }

    /* a ready-made piece of SQL (a join condition): matched as written */
    void raw(const QString &text, Kind k, int group)
    {
        m_c.append({text, text, k, group, false});
    }

    /* what usually comes next after the governing keyword ("SET" after
     * UPDATE t), ahead of everything else — if the dialect has it */
    void likelyNext(const QString &governing, bool afterTable)
    {
        static const QHash<QString, QStringList> afterTableNext = {
            {"UPDATE", {"SET"}},
            {"INTO", {"VALUES", "SELECT"}},
            {"JOIN", {"ON", "USING"}},
            {"STRAIGHT_JOIN", {"ON", "USING"}},
            {"FROM", {"WHERE", "JOIN", "INNER JOIN", "LEFT JOIN", "GROUP BY", "ORDER BY", "LIMIT"}},
            {"TABLE", {"ADD COLUMN", "DROP COLUMN", "MODIFY COLUMN", "RENAME TO"}},
        };
        static const QHash<QString, QStringList> afterOperandNext = {
            {"SELECT", {"FROM", "AS"}},
        };
        const QStringList next = (afterTable ? afterTableNext : afterOperandNext).value(governing);
        for(const QString &w : next)
            for(const Word &k : kKeywords)
                if((k.dialects & m_bit) && w == QLatin1String(k.text)) {
                    m_c.append({w, w, Kind::Keyword, kRemainderGroup, false});
                    break;
                }
    }

    QList<Item> ranked(const QString &prefix) const
    {
        struct Scored
        {
            const Cand *c;
            int tier;
            bool exact;
            int index;
        };
        QList<Scored> s;
        for(int i = 0; i < m_c.size(); ++i) {
            const int tier = matchTier(m_c[i].match, prefix, m_c[i].ident);
            if(tier >= 0)
                s.append(
                    {&m_c[i], tier,
                     !prefix.isEmpty() && m_c[i].match.compare(prefix, Qt::CaseInsensitive) == 0,
                     i});
        }
        /* strong matches (prefix, word boundary) before weak ones (substring,
         * fuzzy); then the context's grouping; then how well; then an exact
         * hit; then list order */
        std::stable_sort(s.begin(), s.end(), [](const Scored &a, const Scored &b) {
            const bool weakA = a.tier >= 2, weakB = b.tier >= 2;
            if(weakA != weakB)
                return weakB;
            if(a.c->group != b.c->group)
                return a.c->group < b.c->group;
            if(a.tier != b.tier)
                return a.tier < b.tier;
            if(a.exact != b.exact)
                return a.exact;
            return a.index < b.index;
        });
        QList<Item> out;
        QSet<QString> seen;
        for(const Scored &x : std::as_const(s)) {
            if(seen.contains(x.c->insert))
                continue;
            seen.insert(x.c->insert);
            out.append({x.c->insert, x.c->kind});
            if(out.size() >= kMaxItems)
                break;
        }
        return out;
    }

private:
public:
    static constexpr int kRemainderGroup = -1;
    static constexpr int kJoinGroup = -2; /* a join condition beats everything */

private:
    static constexpr int kMaxItems = 500;

    QString remainderAfterHead(const QString &phrase) const
    {
        const QStringList parts = phrase.split(QLatin1Char(' '));
        for(int k = int(parts.size()) - 1; k >= 1; --k) {
            if(k > m_head.size())
                continue;
            bool ok = true;
            for(int i = 0; i < k && ok; ++i) {
                const Tok &t = m_head[m_head.size() - k + i];
                ok = t.type == Tok::Name && t.text.compare(parts[i], Qt::CaseInsensitive) == 0;
            }
            if(ok)
                return parts.mid(k).join(QLatin1Char(' '));
        }
        return QString();
    }

public:
    bool m_joinOn = false; /* the caret is in a JOIN … ON condition */

private:
    SqlDriverType m_d;
    quint8 m_bit;
    bool m_userQuoted;
    const QList<Tok> &m_head;
    QList<Cand> m_c;
};

template<size_t N> constexpr int countOf(const Word (&)[N])
{
    return int(N);
}

QStringList without(const QStringList &all, const QStringList &drop)
{
    const QSet<QString> d(drop.cbegin(), drop.cend());
    QStringList out;
    for(const QString &x : all)
        if(!d.contains(x))
            out << x;
    return out;
}

bool sameName(const QString &a, const QString &b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

/* the conditions that join `joined` to `other` through a foreign key in
 * either direction — "j.x = o.y [AND …]", the newly joined table's side
 * first. A self-referencing key on a self join gives both readings. */
QStringList joinConditions(const StmtTable &joined, const StmtTable &other,
                           const QList<ForeignKey> &fks, SqlDriverType d)
{
    const QString ja = quoteIfNeeded(joined.alias.isEmpty() ? joined.table : joined.alias, d);
    const QString oa = quoteIfNeeded(other.alias.isEmpty() ? other.table : other.alias, d);
    const auto cond = [&](const QStringList &jc, const QStringList &oc) {
        QStringList parts;
        for(int k = 0; k < jc.size() && k < oc.size(); ++k)
            parts << QStringLiteral("%1.%2 = %3.%4")
                         .arg(ja, quoteIfNeeded(jc[k], d), oa, quoteIfNeeded(oc[k], d));
        return parts.join(QStringLiteral(" AND "));
    };
    QStringList out;
    for(const ForeignKey &fk : fks) {
        if(sameName(fk.table, joined.table) && sameName(fk.refTable, other.table))
            out << cond(fk.columns, fk.refColumns);
        if(sameName(fk.table, other.table) && sameName(fk.refTable, joined.table))
            out << cond(fk.refColumns, fk.columns);
    }
    return out;
}

/* whether the last ON in `t` is a JOIN's — not INSERT's ON DUPLICATE KEY
 * UPDATE / ON CONFLICT: the nearest JOIN or INSERT-side keyword before it
 * decides */
bool isJoinOn(const QList<Tok> &t)
{
    int i = int(t.size()) - 1;
    while(i >= 0 && !isWord(t[i], "ON"))
        --i;
    for(--i; i >= 0; --i) {
        if(isWord(t[i], "JOIN") || isWord(t[i], "STRAIGHT_JOIN"))
            return true;
        if(isWord(t[i], "VALUES") || isWord(t[i], "VALUE") || isWord(t[i], "INTO") ||
           isWord(t[i], "SET"))
            return false;
    }
    return false;
}

/* tables a foreign key ties to any of `tables`, either direction */
QStringList relatedTables(const QVector<StmtTable> &tables, const QList<ForeignKey> &fks)
{
    QStringList out;
    for(const StmtTable &t : tables)
        for(const ForeignKey &fk : fks) {
            if(sameName(fk.table, t.table))
                out << fk.refTable;
            if(sameName(fk.refTable, t.table))
                out << fk.table;
        }
    out.removeDuplicates();
    return out;
}

} // namespace

QString prefixAt(const QString &sql, int pos)
{
    pos = qBound(0, pos, int(sql.size()));
    int s = pos;
    while(s > 0 && isIdentChar(sql[s - 1]))
        --s;
    return sql.mid(s, pos - s);
}

bool inStringOrComment(const QString &sql, int pos, SqlDriverType driver)
{
    pos = qBound(0, pos, int(sql.size()));
    const StatementSpan span = statementSpanAt(sql, pos, driver);
    EndState st;
    tokenize(sql.mid(span.start, pos - span.start), driver, &st);
    return st == EndState::String || st == EndState::Comment;
}

QList<Item> complete(const QString &sql, int pos, const Schema &schema, SqlDriverType driver)
{
    pos = qBound(0, pos, int(sql.size()));
    const StatementSpan span = statementSpanAt(sql, pos, driver);
    const QString prefix = prefixAt(sql, pos);
    int headEnd = pos - int(prefix.size());
    /* `emp| or "emp| — the user opened the quote: offer raw names */
    const bool userQuoted = headEnd > span.start && (sql[headEnd - 1] == QLatin1Char('`') ||
                                                     sql[headEnd - 1] == QLatin1Char('"'));
    if(userQuoted)
        --headEnd;
    headEnd = qMax(headEnd, span.start);

    const QList<Tok> head = tokenize(sql.mid(span.start, headEnd - span.start), driver);
    const QList<Tok> whole = tokenize(sql.mid(span.start, span.end - span.start), driver);
    const Ctx ctx = analyze(head);
    /* after "3." or a dot nothing resolves: no guesses */
    if(ctx.type != Ctx::Qualified && headEnd > span.start && sql[headEnd - 1] == QLatin1Char('.'))
        return {};
    const QVector<StmtTable> stmtTables = tablesIn(whole, schema);

    Builder b(driver, userQuoted, head);
    const int nKw = countOf(kKeywords), nFn = countOf(kFunctions), nTy = countOf(kTypes);
    const QStringList tablesAndViews = schema.tables + schema.views;

    /* the statement's own tables' columns, in table then ordinal order */
    QStringList own;
    if(schema.columnsOf)
        for(const StmtTable &t : stmtTables)
            own += schema.columnsOf(t.table);
    own.removeDuplicates();
    QStringList aliases;
    for(const StmtTable &t : stmtTables)
        aliases << (t.alias.isEmpty() ? t.table : t.alias);
    aliases.removeDuplicates();
    const QStringList columnPool = schema.columns.isEmpty() ? schema.extra : schema.columns;

    const auto addTables = [&](int group) {
        if(tablesAndViews.isEmpty()) {
            b.idents(schema.extra, Kind::Table, group);
            return;
        }
        b.idents(schema.tables, Kind::Table, group);
        b.idents(schema.views, Kind::View, group);
    };
    const auto addExpression = [&](int first) {
        b.idents(own, Kind::Column, first, false);
        b.idents(aliases, Kind::Alias, first + 1, false);
        b.idents(without(columnPool, own), Kind::Column, first + 2);
        b.idents(schema.functions, Kind::Function, first + 3, true, QStringLiteral("("));
        b.words(kFunctions, nFn, Clause, Kind::Function, first + 3);
    };

    switch(ctx.type) {
        case Ctx::Qualified: {
            QStringList cols;
            QString table;
            for(const StmtTable &t : stmtTables)
                if(ctx.name.compare(t.alias, Qt::CaseInsensitive) == 0 ||
                   ctx.name.compare(t.table, Qt::CaseInsensitive) == 0) {
                    table = t.table;
                    break;
                }
            if(table.isEmpty())
                table = canonical(tablesAndViews, ctx.name);
            if(!table.isEmpty()) {
                if(schema.columnsOf)
                    cols = schema.columnsOf(table);
                b.idents(cols, Kind::Column, 0, false);
            } else if(!canonical(schema.databases, ctx.name).isEmpty()) {
                const QString db = canonical(schema.databases, ctx.name);
                if(db.compare(schema.database, Qt::CaseInsensitive) == 0) {
                    b.idents(schema.tables, Kind::Table, 0);
                    b.idents(schema.views, Kind::View, 0);
                } else if(schema.tablesOf) {
                    b.idents(schema.tablesOf(db), Kind::Table, 0);
                }
            }
            break;
        }
        case Ctx::Table:
            if(ctx.name.endsWith(QLatin1String("JOIN"))) {
                const QStringList related =
                    relatedTables(tablesIn(head, schema), schema.foreignKeys);
                QStringList known;
                for(const QString &r : related)
                    if(!canonical(tablesAndViews, r).isEmpty())
                        known << canonical(tablesAndViews, r);
                b.idents(known, Kind::Table, Builder::kRemainderGroup);
            }
            addTables(0);
            b.idents(schema.databases, Kind::Database, 1);
            b.words(kKeywords, nKw, Clause, Kind::Keyword, 2);
            break;
        case Ctx::AfterTable:
            b.likelyNext(ctx.name, true);
            b.words(kKeywords, nKw, Clause, Kind::Keyword, 0);
            addTables(1);
            break;
        case Ctx::Column:
            if(ctx.name == QLatin1String("ON") && isJoinOn(head)) {
                b.m_joinOn = true;
                const QVector<StmtTable> before = tablesIn(head, schema);
                for(int i = 0; i + 1 < before.size(); ++i)
                    for(const QString &c :
                        joinConditions(before.last(), before[i], schema.foreignKeys, driver))
                        b.raw(c, Kind::Join, Builder::kJoinGroup);
            }
            addExpression(0);
            b.words(kKeywords, nKw, Clause, Kind::Keyword, 4);
            break;
        case Ctx::AfterOperand:
            b.likelyNext(ctx.name, false);
            b.words(kKeywords, nKw, Clause, Kind::Keyword, 0);
            addExpression(1);
            break;
        case Ctx::InsertColumns:
        case Ctx::TableColumns: {
            QStringList cols;
            const QString table = canonical(tablesAndViews, ctx.name);
            if(!table.isEmpty() && schema.columnsOf)
                cols = schema.columnsOf(table);
            b.idents(cols, Kind::Column, 0, false);
            if(cols.isEmpty())
                b.idents(columnPool, Kind::Column, 1);
            if(ctx.type == Ctx::TableColumns)
                b.words(kKeywords, nKw, Clause, Kind::Keyword, 2);
            break;
        }
        case Ctx::Values:
            b.idents(schema.functions, Kind::Function, 0, true, QStringLiteral("("));
            b.words(kFunctions, nFn, Clause, Kind::Function, 0);
            b.words(kKeywords, nKw, Clause, Kind::Keyword, 1);
            break;
        case Ctx::Database:
            b.idents(schema.databases, Kind::Database, 0);
            break;
        case Ctx::Procedure:
            b.idents(schema.procedures, Kind::Procedure, 0, true, QStringLiteral("("));
            break;
        case Ctx::DdlColumns:
            b.words(kTypes, nTy, Clause, Kind::Keyword, 0);
            b.words(kKeywords, nKw, Clause, Kind::Keyword, 1);
            break;
        case Ctx::Nothing:
            break;
        case Ctx::Any:
            b.words(kKeywords, nKw, Start, Kind::Keyword, 0);
            addTables(1);
            b.words(kKeywords, nKw, Clause, Kind::Keyword, 2);
            b.words(kFunctions, nFn, Clause, Kind::Function, 3);
            if(!tablesAndViews.isEmpty())
                b.idents(schema.extra, Kind::Column, 4);
            break;
    }
    return b.ranked(prefix);
}

QString quoteIfNeeded(const QString &ident, SqlDriverType driver)
{
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z_][A-Za-z0-9_$]*$"));
    const bool needs = !plain.match(ident).hasMatch() ||
                       reservedWords().contains(ident.toUpper()) ||
                       (driver == SqlDriverType::Postgres && ident != ident.toLower());
    if(!needs)
        return ident;
    if(driver == SqlDriverType::Mysql)
        return QLatin1Char('`') + QString(ident).replace(QLatin1Char('`'), QStringLiteral("``")) +
               QLatin1Char('`');
    return QLatin1Char('"') + QString(ident).replace(QLatin1Char('"'), QStringLiteral("\"\"")) +
           QLatin1Char('"');
}

} // namespace SqlCompletion
