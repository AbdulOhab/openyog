#include "ResultExport.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTextStream>

namespace ResultExport {
namespace {
bool isNull(const QString &v, const Options &opt)
{
    return v == opt.nullText || v == QStringLiteral("NULL");
}

QString xmlEsc(QString s)
{
    return s.replace('&', QStringLiteral("&amp;"))
        .replace('<', QStringLiteral("&lt;"))
        .replace('>', QStringLiteral("&gt;"))
        .replace('"', QStringLiteral("&quot;"));
}

QString htmlEsc(QString s)
{
    return s.replace('&', QStringLiteral("&amp;"))
        .replace('<', QStringLiteral("&lt;"))
        .replace('>', QStringLiteral("&gt;"));
}

/* a single-quoted string literal. MySQL treats backslash as an escape
 * character in string literals by default, so a literal backslash/newline/CR
 * needs its own escape sequence to round-trip; Postgres and SQLite don't
 * (standard_conforming_strings, on by default for decades now, means a
 * backslash there is just a backslash) — escaping it there anyway would
 * leave a literal two-character "\n" in the re-imported data instead of a
 * real newline, and "\\'" would actually end the string early rather than
 * escape the quote. ansi=true instead doubles the quote (the one escape
 * ANSI SQL actually defines) and leaves backslash/newline/CR untouched, as
 * a real newline embedded in a quoted literal spanning lines. */
QString sqlLit(QString s, bool ansi)
{
    if(!ansi) {
        s.replace('\\', QStringLiteral("\\\\"));
        s.replace('\'', QStringLiteral("\\'"));
        s.replace('\n', QStringLiteral("\\n"));
        s.replace('\r', QStringLiteral("\\r"));
        return QLatin1Char('\'') + s + QLatin1Char('\'');
    }
    s.replace('\'', QStringLiteral("''"));
    return QLatin1Char('\'') + s + QLatin1Char('\'');
}

QString delimited(const QStringList &headers, const std::function<QString(int, int)> &cell,
                  int rows, int cols, const Options &opt, QChar sep)
{
    const QString q = QString(opt.quote);
    const auto field = [&](QString s) {
        if(!q.isEmpty() &&
           (s.contains(sep) || s.contains(opt.quote) || s.contains('\n') || s.contains('\r')))
            return q + s.replace(opt.quote, QString(opt.quote) + opt.quote) + q;
        return s;
    };
    QString out;
    if(opt.header) {
        QStringList h;
        for(const QString &x : headers)
            h << field(x);
        out += h.join(sep) + opt.lineEnd;
    }
    for(int r = 0; r < rows; ++r) {
        QStringList row;
        for(int c = 0; c < cols; ++c)
            row << field(cell(r, c));
        out += row.join(sep) + opt.lineEnd;
    }
    return out;
}
} // namespace

QString render(Format fmt, const QStringList &headers, const std::function<QString(int, int)> &cell,
               int rows, int cols, const Options &opt)
{
    QString buf;
    QTextStream out(&buf);

    switch(fmt) {
        case Format::Csv:
            if(opt.bom)
                out << QChar(0xFEFF);
            out << delimited(headers, cell, rows, cols, opt, opt.delimiter);
            break;
        case Format::Tsv:
            if(opt.bom)
                out << QChar(0xFEFF);
            out << delimited(headers, cell, rows, cols, opt, QLatin1Char('\t'));
            break;

        case Format::Html: {
            out << "<!doctype html><meta charset=\"utf-8\">\n"
                   "<style>table{border-collapse:collapse;font:13px sans-serif}"
                   "th,td{border:1px solid #ccc;padding:3px 7px}"
                   "th{background:#3B7DBB;color:#fff}</style>\n<table>\n";
            if(opt.header) {
                out << "<tr>";
                for(const QString &h : headers)
                    out << "<th>" << htmlEsc(h) << "</th>";
                out << "</tr>\n";
            }
            for(int r = 0; r < rows; ++r) {
                out << "<tr>";
                for(int c = 0; c < cols; ++c)
                    out << "<td>" << htmlEsc(cell(r, c)) << "</td>";
                out << "</tr>\n";
            }
            out << "</table>\n";
            break;
        }

        case Format::Json: {
            QJsonArray arr;
            for(int r = 0; r < rows; ++r) {
                QJsonObject o;
                for(int c = 0; c < cols; ++c) {
                    const QString v = cell(r, c);
                    o.insert(headers.value(c, QString::number(c)),
                             isNull(v, opt) ? QJsonValue(QJsonValue::Null) : QJsonValue(v));
                }
                arr.append(o);
            }
            out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Indented));
            break;
        }

        case Format::Markdown: {
            const auto esc = [](QString s) { return s.replace('|', QStringLiteral("\\|")); };
            QStringList h;
            for(const QString &x : headers)
                h << esc(x);
            out << "| " << h.join(QStringLiteral(" | ")) << " |\n";
            out << "|" << QString(QStringLiteral(" --- |")).repeated(qMax(1, cols)) << "\n";
            for(int r = 0; r < rows; ++r) {
                QStringList row;
                for(int c = 0; c < cols; ++c)
                    row << esc(cell(r, c));
                out << "| " << row.join(QStringLiteral(" | ")) << " |\n";
            }
            break;
        }

        case Format::Xml: {
            out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<rows xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n";
            for(int r = 0; r < rows; ++r) {
                out << "  <row>\n";
                for(int c = 0; c < cols; ++c) {
                    const QString tag = xmlEsc(headers.value(c, QString::number(c)))
                                            .replace(' ', QStringLiteral("_"));
                    const QString v = cell(r, c);
                    if(isNull(v, opt))
                        out << "    <" << tag << " xsi:nil=\"true\"/>\n";
                    else
                        out << "    <" << tag << ">" << xmlEsc(v) << "</" << tag << ">\n";
                }
                out << "  </row>\n";
            }
            out << "</rows>\n";
            break;
        }

        case Format::Sql: {
            /* matches opt.sqlCreate's own dialect (the caller fills that in via
             * IDbConnection::showCreate(), already quoted correctly) rather
             * than always assuming backticks */
            const bool ansiQuote = opt.driver != DriverType::Mysql;
            const auto qi = [&](const QString &s) {
                return ansiQuote
                           ? QLatin1Char('"') + QString(s).replace('"', QStringLiteral("\"\"")) +
                                 QLatin1Char('"')
                           : QLatin1Char('`') + QString(s).replace('`', QStringLiteral("``")) +
                                 QLatin1Char('`');
            };
            const QString tbl = opt.sqlTable;
            if(opt.sqlStructure && !opt.sqlCreate.trimmed().isEmpty()) {
                out << "DROP TABLE IF EXISTS " << qi(tbl) << ";\n" << opt.sqlCreate.trimmed();
                if(!opt.sqlCreate.trimmed().endsWith(QLatin1Char(';')))
                    out << ";";
                out << "\n\n";
            }
            QStringList colList;
            for(const QString &h : headers)
                colList << qi(h);
            const QString prefix = QStringLiteral("INSERT INTO %1 (%2) VALUES\n")
                                       .arg(qi(tbl), colList.join(QStringLiteral(", ")));
            for(int r = 0; r < rows; ++r) {
                if(r % 200 == 0) {
                    if(r)
                        out << ";\n";
                    out << prefix;
                } else {
                    out << ",\n";
                }
                QStringList vals;
                for(int c = 0; c < cols; ++c) {
                    const QString v = cell(r, c);
                    vals << (isNull(v, opt) ? QStringLiteral("NULL") : sqlLit(v, ansiQuote));
                }
                out << "  (" << vals.join(QStringLiteral(", ")) << ")";
            }
            if(rows)
                out << ";\n";
            break;
        }

        case Format::Excel: {
            /* SpreadsheetML 2003 — plain XML, opens in Excel/LibreOffice, no zip */
            out << "<?xml version=\"1.0\"?>\n"
                   "<?mso-application progid=\"Excel.Sheet\"?>\n"
                   "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" "
                   "xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">\n"
                   " <Worksheet ss:Name=\"Result\">\n  <Table>\n";
            const auto rowXml = [&](const QStringList &cells, bool head) {
                out << "   <Row>\n";
                for(const QString &v : cells)
                    out << "    <Cell><Data ss:Type=\"" << (head ? "String" : "String") << "\">"
                        << xmlEsc(v) << "</Data></Cell>\n";
                out << "   </Row>\n";
            };
            if(opt.header)
                rowXml(headers, true);
            for(int r = 0; r < rows; ++r) {
                QStringList row;
                for(int c = 0; c < cols; ++c)
                    row << cell(r, c);
                rowXml(row, false);
            }
            out << "  </Table>\n </Worksheet>\n</Workbook>\n";
            break;
        }
    }

    out.flush();
    return buf;
}

bool write(const QString &path, Format fmt, const QStringList &headers,
           const std::function<QString(int, int)> &cell, int rows, int cols, const Options &opt,
           QString *err)
{
    QFile f(path);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if(err)
            *err = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << render(fmt, headers, cell, rows, cols, opt);
    out.flush();
    if(f.error() != QFileDevice::NoError) {
        if(err)
            *err = f.errorString();
        return false;
    }
    return true;
}

Format formatForName(const QString &name)
{
    const QString n = name.toUpper();
    if(n.startsWith(QStringLiteral("TSV")))
        return Format::Tsv;
    if(n.startsWith(QStringLiteral("HTML")))
        return Format::Html;
    if(n.startsWith(QStringLiteral("JSON")))
        return Format::Json;
    if(n.startsWith(QStringLiteral("MARKDOWN")))
        return Format::Markdown;
    if(n.startsWith(QStringLiteral("XML")))
        return Format::Xml;
    if(n.startsWith(QStringLiteral("SQL")))
        return Format::Sql;
    if(n.startsWith(QStringLiteral("EXCEL")))
        return Format::Excel;
    return Format::Csv;
}

QString suffixFor(Format f)
{
    switch(f) {
        case Format::Tsv:
            return QStringLiteral("tsv");
        case Format::Html:
            return QStringLiteral("html");
        case Format::Json:
            return QStringLiteral("json");
        case Format::Markdown:
            return QStringLiteral("md");
        case Format::Xml:
            return QStringLiteral("xml");
        case Format::Sql:
            return QStringLiteral("sql");
        case Format::Excel:
            return QStringLiteral("xls");
        case Format::Csv:
            break;
    }
    return QStringLiteral("csv");
}

QStringList formatNames()
{
    return {QStringLiteral("CSV (comma-separated)"),
            QStringLiteral("TSV (tab-separated)"),
            QStringLiteral("HTML table"),
            QStringLiteral("JSON"),
            QStringLiteral("Markdown table"),
            QStringLiteral("XML"),
            QStringLiteral("SQL INSERT statements"),
            QStringLiteral("Excel (SpreadsheetML .xls)")};
}
} // namespace ResultExport
