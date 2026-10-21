/* OpenYog — split a script into individual statements on the current
 * terminator, honouring `DELIMITER xxx` lines (so a CREATE PROCEDURE …
 * BEGIN … END stays one statement). DELIMITER lines are consumed, not
 * emitted.
 *
 * A terminator only counts outside quoted text and comments, read the way
 * the target server reads them:
 *   - '…' strings, "…" strings/identifiers and `…` identifiers, with a
 *     doubled quote staying inside; backslash escapes too on MySQL, and in
 *     PostgreSQL E'…' strings
 *   - -- line comments (MySQL: only when followed by whitespace, so 1--1 is
 *     still arithmetic), # line comments on MySQL, block comments everywhere
 *   - PostgreSQL $$ … $$ / $tag$ … $tag$ dollar-quoted bodies
 * Comments stay attached to the statement they sit in; a stretch holding
 * nothing but whitespace and comments isn't emitted as a statement. */
#pragma once
#include "ConnectionParams.h"

#include <QString>
#include <QStringList>

QStringList splitStatements(const QString &sql, SqlDriverType driver = SqlDriverType::Mysql);

/* the single statement whose text range covers character offset `pos`
 * (same rules as splitStatements). A caret on blank or comment-only text
 * picks the nearest statement before it. Empty if none. */
QString statementAt(const QString &sql, int pos, SqlDriverType driver = SqlDriverType::Mysql);

/* [start, end) of the raw statement text the caret at `pos` sits in, up to
 * (not including) its terminator: unlike statementAt(), no trimming and no
 * falling back to an earlier statement — what the editor's autocomplete
 * needs to read the clause around the caret. A caret on a DELIMITER line
 * gets the empty span {pos, pos}. */
struct StatementSpan
{
    int start = 0;
    int end = 0;
};
StatementSpan statementSpanAt(const QString &sql, int pos,
                              SqlDriverType driver = SqlDriverType::Mysql);
