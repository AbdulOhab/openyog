#include "SchemaSql.h"

#include <QRegularExpression>

namespace SchemaSql {

namespace {
QString qi(SqlDriverType driver, const QString &ident)
{
    if(driver == SqlDriverType::Postgres)
        return QLatin1Char('"') + QString(ident).replace(QLatin1Char('"'), QStringLiteral("\"\"")) +
               QLatin1Char('"');
    return QLatin1Char('`') + QString(ident).replace(QLatin1Char('`'), QStringLiteral("``")) +
           QLatin1Char('`');
}
} // namespace

QString stripDefiner(const QString &ddl)
{
    QString s = ddl;
    /* DEFINER=`user`@`host`  (the usual SHOW CREATE form) */
    s.remove(QRegularExpression(QStringLiteral("DEFINER\\s*=\\s*`[^`]*`@`[^`]*`\\s*")));
    /* fallback: DEFINER=user@host with no backticks */
    s.remove(QRegularExpression(QStringLiteral("DEFINER\\s*=\\s*[^ \\t]+@[^ \\t]+\\s*")));
    return s;
}

QString createTemplate(const QString &objType, const QString &db, SqlDriverType driver)
{
    const QString d = qi(driver, db);

    if(driver == SqlDriverType::Postgres) {
        if(objType == QStringLiteral("VIEW"))
            return QStringLiteral("CREATE VIEW %1.\"new_view\" AS\nSELECT 1 AS n;").arg(d);
        if(objType == QStringLiteral("PROCEDURE"))
            return QStringLiteral("CREATE PROCEDURE %1.\"new_proc\"(IN arg1 INT)\n"
                                  "  LANGUAGE plpgsql\n"
                                  "AS $body$\n"
                                  "BEGIN\n"
                                  "  \n"
                                  "END;\n"
                                  "$body$")
                .arg(d);
        if(objType == QStringLiteral("FUNCTION"))
            return QStringLiteral("CREATE FUNCTION %1.\"new_func\"(arg1 INT)\n"
                                  "  RETURNS INT\n"
                                  "  LANGUAGE plpgsql\n"
                                  "AS $body$\n"
                                  "BEGIN\n"
                                  "  RETURN arg1;\n"
                                  "END;\n"
                                  "$body$")
                .arg(d);
        if(objType == QStringLiteral("TRIGGER"))
            /* two statements — a trigger function, then the trigger that
             * calls it — sent together (see SchemaSql.h); the second
             * statement has no compound body of its own, so only the
             * function part needs the $body$ dollar-quoting */
            return QStringLiteral("CREATE FUNCTION %1.\"new_trigger_fn\"()\n"
                                  "  RETURNS TRIGGER\n"
                                  "  LANGUAGE plpgsql\n"
                                  "AS $body$\n"
                                  "BEGIN\n"
                                  "  \n"
                                  "  RETURN NEW;\n"
                                  "END;\n"
                                  "$body$;\n"
                                  "\n"
                                  "CREATE TRIGGER \"new_trigger\"\n"
                                  "  BEFORE INSERT ON \"some_table\"\n"
                                  "  FOR EACH ROW EXECUTE FUNCTION %1.\"new_trigger_fn\"();")
                .arg(d);
        /* EVENT: no PostgreSQL equivalent — ConnectionTab guards this before
         * ever reaching here, same as its other capability-gap guards */
        return QString();
    }

    if(objType == QStringLiteral("VIEW"))
        return QStringLiteral("CREATE VIEW %1.%2 AS\nSELECT 1 AS n;")
            .arg(d, qi(driver, QStringLiteral("new_view")));
    if(objType == QStringLiteral("PROCEDURE"))
        return QStringLiteral("CREATE PROCEDURE %1.%2(IN arg1 INT)\nBEGIN\n  \nEND")
            .arg(d, qi(driver, QStringLiteral("new_proc")));
    if(objType == QStringLiteral("FUNCTION"))
        return QStringLiteral("CREATE FUNCTION %1.%2(arg1 INT)\n  RETURNS INT\n"
                              "  DETERMINISTIC\nBEGIN\n  RETURN arg1;\nEND")
            .arg(d, qi(driver, QStringLiteral("new_func")));
    if(objType == QStringLiteral("TRIGGER"))
        return QStringLiteral("CREATE TRIGGER %1.%2\n  BEFORE INSERT ON %3\n"
                              "  FOR EACH ROW\nBEGIN\n  \nEND")
            .arg(d, qi(driver, QStringLiteral("new_trigger")),
                 qi(driver, QStringLiteral("some_table")));
    if(objType == QStringLiteral("EVENT"))
        return QStringLiteral("CREATE EVENT %1.%2\n  ON SCHEDULE EVERY 1 DAY\n"
                              "  DO\nBEGIN\n  \nEND")
            .arg(d, qi(driver, QStringLiteral("new_event")));
    return QString();
}

int showCreateColumn(const QString &objType)
{
    if(objType == QStringLiteral("VIEW"))
        return 1;
    if(objType == QStringLiteral("EVENT"))
        return 3;
    return 2; /* PROCEDURE / FUNCTION / TRIGGER */
}

QString editorText(const QString &objType, const QString &db, const QString &name,
                   const QString &createSql, bool create, SqlDriverType driver)
{
    const QString d = qi(driver, db);
    const QString n = qi(driver, name);
    QString body = createSql.trimmed();
    /* drop a trailing ; the SHOW CREATE / template may carry */
    while(body.endsWith(QLatin1Char(';')))
        body.chop(1);

    if(objType == QStringLiteral("VIEW")) {
        if(!create) {
            static const QRegularExpression lead(
                QStringLiteral("^\\s*CREATE\\s+(?:OR\\s+REPLACE\\s+)?"),
                QRegularExpression::CaseInsensitiveOption);
            body.replace(lead, QStringLiteral("CREATE OR REPLACE "));
        }
        return body + QLatin1Char(';') + QLatin1Char('\n');
    }

    if(driver == SqlDriverType::Postgres) {
        if(!create &&
           (objType == QStringLiteral("FUNCTION") || objType == QStringLiteral("PROCEDURE"))) {
            /* showCreate() already returns "CREATE OR REPLACE …" (that's
             * what pg_get_functiondef() itself produces) — no DROP needed,
             * just re-run it; still needs the DELIMITER wrap below since
             * the body's own semicolons (inside $tag$ … $tag$) would
             * otherwise confuse the plain ';'-splitting editor. */
            /* the newline before the closing $$ matters: body ends in the
             * dollar-quote's own closing tag (…$tag$), and a scanner that
             * just looks for the literal two-character substring "$$" (see
             * SqlSplit.cpp) matches one character too early without it —
             * the tag's last '$' plus the first '$' of our own delimiter
             * — truncating the tag and leaving a stray trailing '$' as its
             * own bogus statement. Confirmed via --pgschematest=: without
             * this newline PROCEDURE/FUNCTION create failed with "syntax
             * error at or near '$'". */
            return QStringLiteral("DELIMITER $$\n\n%1\n$$\n\nDELIMITER ;\n").arg(body);
        }
        if(!create && objType == QStringLiteral("TRIGGER")) {
            /* Postgres has no reliably-available "CREATE OR REPLACE
             * TRIGGER" (added only in PG 14+) — DROP + CREATE instead,
             * portable to older servers. DROP TRIGGER needs "ON table",
             * which alterSchemaObject() has no parameter for — pulled out
             * of the DDL text itself (pg_get_triggerdef() always includes
             * "ON <table> FOR EACH …", and pg_get_triggerdef() only quotes
             * an identifier that actually needs it, so the schema-prefix
             * alternative below has to accept a bare unquoted "schema."
             * just as much as a quoted one). No DELIMITER wrap: a trigger
             * definition has no compound body of its own to protect. */
            static const QRegularExpression tableRe(
                QStringLiteral("\\bON\\s+((?:(?:\"[^\"]+\"|[A-Za-z_][\\w$]*)\\.)?"
                               "(?:\"[^\"]+\"|[A-Za-z_][\\w$]*))\\s+FOR\\b"),
                QRegularExpression::CaseInsensitiveOption);
            const auto m = tableRe.match(body);
            QString out;
            if(m.hasMatch())
                out += QStringLiteral("DROP TRIGGER IF EXISTS %1 ON %2;\n\n").arg(n, m.captured(1));
            out += body + QStringLiteral(";\n");
            return out;
        }
        /* create path (or VIEW, already handled above): FUNCTION/
         * PROCEDURE/TRIGGER templates all carry a $body$-quoted compound
         * body (TRIGGER's is two statements — see createTemplate()) that
         * needs the same DELIMITER protection MySQL's routines get. */
        return QStringLiteral("DELIMITER $$\n\n%1\n$$\n\nDELIMITER ;\n").arg(body);
    }

    /* MySQL/SQLite: no ALTER for routines/triggers/events — DROP … IF
     * EXISTS + CREATE, wrapped so the compound body stays one statement */
    QString out = QStringLiteral("DELIMITER $$\n\n");
    if(!create)
        out += QStringLiteral("DROP %1 IF EXISTS %2.%3$$\n\n").arg(objType, d, n);
    out += body + QStringLiteral("\n$$\n\nDELIMITER ;\n");
    return out;
}

} // namespace SchemaSql
