/* OpenYog — split a script into individual statements on the current
 * terminator, honouring `DELIMITER xxx` lines (so a CREATE PROCEDURE …
 * BEGIN … END stays one statement) and single-quoted strings.  DELIMITER
 * lines are consumed, not emitted. */
#pragma once

#include <QString>
#include <QStringList>

QStringList splitStatements(const QString &sql);
