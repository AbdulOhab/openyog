/* OpenYog — one place that turns a grid of strings into an export file.
 * Backs both the query-result "Export result as…" and the Table Data pane's
 * "Export…" button.  Mirrors SQLyog's ExportData / ExportAsSQL / ExportToExcel
 * format set; no external deps (XML, SpreadsheetML and SQL are emitted by
 * hand). */
#pragma once

#include "ConnectionParams.h"

#include <QChar>
#include <QString>
#include <QStringList>

#include <functional>

namespace ResultExport {
enum class Format { Csv, Tsv, Html, Json, Markdown, Xml, Sql, Excel };

struct Options
{
    QChar delimiter = QLatin1Char(','); /* CSV only */
    QChar quote = QLatin1Char('"');     /* CSV only */
    QString lineEnd = QStringLiteral("\r\n");
    QString nullText = QStringLiteral("NULL");
    bool header = true;
    bool bom = false;                              /* UTF-8 BOM (CSV/TSV) */
    QString sqlTable = QStringLiteral("exported"); /* SQL INSERT target */
    bool sqlStructure = false;                     /* SQL: emit DROP + CREATE first */
    QString sqlCreate;                             /* the CREATE TABLE statement */
    /* SQL format only: picks identifier quoting (backtick vs double-quote)
     * to match sqlCreate's own dialect — the source connection's driver,
     * not necessarily the driver the exported file will ever be run against */
    SqlDriverType driver = SqlDriverType::Mysql;
};

/* cell(row, col) returns the display string; a value equal to Options::nullText
 * (or the literal "NULL") is treated as SQL NULL where the format distinguishes
 * it. Returns false + fills *err on I/O failure. */
/* the export as a string — same content write() would put in the file */
QString render(Format fmt, const QStringList &headers, const std::function<QString(int, int)> &cell,
               int rows, int cols, const Options &opt);

bool write(const QString &path, Format fmt, const QStringList &headers,
           const std::function<QString(int, int)> &cell, int rows, int cols, const Options &opt,
           QString *err);

Format formatForName(const QString &name); /* "CSV" → Csv, … */
QString suffixFor(Format f);               /* Csv → "csv" */
QStringList formatNames();                 /* for a combo box */
} // namespace ResultExport
