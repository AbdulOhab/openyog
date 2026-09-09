/* OpenYog — a lightweight SQL pretty-printer (Edit > SQL Formatter, F12).
 * Heuristic, not a parser: it tokenises (respecting '…' strings, `…`
 * identifiers and -- / #  / block comments), upper-cases recognised keywords,
 * and re-lays major clauses onto their own lines with paren-depth indenting.
 * Good enough for everyday SELECT / INSERT / UPDATE / DELETE; leaves anything
 * it doesn't understand alone. */
#pragma once

#include <QString>

namespace SqlFormat {

/* Format one or more `;`-separated statements. */
QString pretty(const QString &sql);

} // namespace SqlFormat
