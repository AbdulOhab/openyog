/* OpenYog — minimal CSV/TSV-style parser behind Import ▸ CSV on SQLite and
 * PostgreSQL connections (ConnectionTab::importCsvBatched), which unlike
 * MySQL's LOAD DATA have no server-side loader to hand the raw
 * separator/terminator strings to.
 *
 * Honours a quote char (with doubled-quote escaping) and a separate escape
 * char with MySQL's FIELDS ESCAPED BY meaning — <esc>n/r/t/0/b/Z are control
 * characters, <esc> before anything else makes it literal — in or out of
 * quotes, so ResultExport's "Escaped by" output reads back unchanged. Both
 * \n and \r\n end a line regardless of the dialog's "Line separator" pick.
 * A null `quote` / `esc` disables that feature. */
#pragma once

#include <QChar>
#include <QList>
#include <QString>
#include <QStringList>

QList<QStringList> parseDelimitedText(const QString &text, QChar sep, QChar quote, QChar esc);
