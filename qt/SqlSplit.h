/* OpenYog — split a script into individual statements on the current
 * terminator, honouring `DELIMITER xxx` lines (so a CREATE PROCEDURE …
 * BEGIN … END stays one statement) and single-quoted strings.  DELIMITER
 * lines are consumed, not emitted. */
#pragma once

#include <QString>
#include <QStringList>

QStringList splitStatements(const QString &sql);

/* the single statement whose text range covers character offset `pos`
 * (DELIMITER-aware, same rules as splitStatements). Empty if none. */
QString statementAt(const QString &sql, int pos);
