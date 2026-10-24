/* OpenYog — Import External Data's reading and writing; see ExternalData.h. */
#include "ExternalData.h"
#include "SqlDump.h"
#include "db/IDbConnection.h"

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTimeZone>
#include <QXmlStreamReader>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ExternalData {

namespace {

/* names for a source's columns: a header's, made unique; "column N" for
 * blank ones */
QStringList columnNames(const QStringList &header)
{
    QStringList out;
    QSet<QString> seen;
    for(int i = 0; i < header.size(); ++i) {
        QString name = header[i].trimmed();
        if(name.isEmpty())
            name = QStringLiteral("column%1").arg(i + 1);
        const QString base = name;
        for(int n = 2; seen.contains(name.toLower()); ++n)
            name = QStringLiteral("%1_%2").arg(base).arg(n);
        seen.insert(name.toLower());
        out << name;
    }
    return out;
}

QStringList numbered(int count)
{
    QStringList out;
    for(int i = 0; i < count; ++i)
        out << QStringLiteral("column%1").arg(i + 1);
    return out;
}

} // namespace

/* ---- CSV ------------------------------------------------------------- */

QChar detectSeparator(const QString &sample, QChar quote)
{
    static const QString candidates = QStringLiteral(",;\t|");
    QList<QList<int>> counts(candidates.size()); /* per candidate, per line */
    QList<int> line(candidates.size(), 0);
    bool inQuotes = false;
    int lines = 0;
    for(int i = 0; i < sample.size() && lines < 20; ++i) {
        const QChar c = sample[i];
        if(!quote.isNull() && c == quote) {
            inQuotes = !inQuotes;
        } else if(!inQuotes && (c == QLatin1Char('\n') || i + 1 == sample.size())) {
            for(int k = 0; k < candidates.size(); ++k)
                counts[k] << line[k];
            line.fill(0);
            ++lines;
        } else if(!inQuotes) {
            const int k = int(candidates.indexOf(c));
            if(k >= 0)
                ++line[k];
        }
    }
    /* the one on every line the same number of times, most often; failing
     * that, the most frequent */
    int best = -1, bestCount = 0;
    for(int k = 0; k < candidates.size(); ++k) {
        const QList<int> &c = counts[k];
        if(c.isEmpty() || c.first() == 0)
            continue;
        const bool steady = std::all_of(c.begin(), c.end(), [&](int n) { return n == c.first(); });
        if(steady && c.first() > bestCount) {
            best = k;
            bestCount = c.first();
        }
    }
    if(best < 0) {
        int most = 0;
        for(int k = 0; k < candidates.size(); ++k) {
            int total = 0;
            for(int n : counts[k])
                total += n;
            if(total > most) {
                most = total;
                best = k;
            }
        }
    }
    return best < 0 ? QLatin1Char(',') : candidates[best];
}

Reader csvReader(const QString &file, const CsvOptions &options)
{
    return [file, options](const OnColumns &onColumns, const EachRow &each, QString *error) {
        QFile f(file);
        if(!f.open(QIODevice::ReadOnly)) {
            if(error)
                *error = QStringLiteral("Can't read %1: %2").arg(file, f.errorString());
            return false;
        }
        const QStringConverter::Encoding encoding =
            options.encoding == CsvOptions::Utf16    ? QStringConverter::Utf16
            : options.encoding == CsvOptions::Latin1 ? QStringConverter::Latin1
                                                     : QStringConverter::Utf8;
        QChar sep = options.separator;
        if(sep.isNull()) {
            QStringDecoder sample(encoding);
            sep = detectSeparator(sample(f.peek(64 * 1024)), options.quote);
        }
        const QChar quote = options.quote;
        QStringDecoder decode(encoding);

        QStringList fields;
        QList<bool> quoted;
        QString field;
        bool inQuotes = false, quoteSeen = false, fieldQuoted = false;
        bool haveColumns = false, stopped = false;
        int width = 0;
        const auto endField = [&] {
            fields << field;
            quoted << fieldQuoted;
            field.clear();
            fieldQuoted = false;
        };
        const auto endRow = [&] {
            endField();
            /* a blank line isn't a row */
            const bool blank = fields.size() == 1 && fields[0].isEmpty() && !quoted[0];
            if(!blank) {
                if(!haveColumns) {
                    haveColumns = true;
                    width = int(fields.size());
                    if(onColumns)
                        onColumns(options.header ? columnNames(fields) : numbered(width));
                    if(options.header) {
                        fields.clear();
                        quoted.clear();
                        return;
                    }
                }
                Row row;
                row.values.resize(width);
                row.nulls.resize(width);
                for(int i = 0; i < width; ++i) {
                    const bool present = i < fields.size();
                    row.values[i] = present ? fields[i].toUtf8() : QByteArray();
                    row.nulls[i] =
                        !present || (options.emptyIsNull && fields[i].isEmpty() && !quoted[i]);
                }
                stopped = !each(row);
            }
            fields.clear();
            quoted.clear();
        };

        bool lastWasCr = false;
        while(!f.atEnd() && !stopped) {
            const QString chunk = decode(f.read(256 * 1024));
            for(qsizetype i = 0; i < chunk.size() && !stopped; ++i) {
                const QChar c = chunk[i];
                if(quoteSeen) { /* a quote inside quotes: doubled, or the end */
                    quoteSeen = false;
                    if(c == quote) {
                        field += c;
                        continue;
                    }
                    inQuotes = false;
                }
                if(inQuotes) {
                    if(c == quote)
                        quoteSeen = true;
                    else
                        field += c;
                    continue;
                }
                if(c == QLatin1Char('\n') && lastWasCr) {
                    lastWasCr = false;
                    continue; /* \r\n: the \r ended the row */
                }
                lastWasCr = c == QLatin1Char('\r');
                if(!quote.isNull() && c == quote && field.isEmpty() && !fieldQuoted) {
                    inQuotes = true;
                    fieldQuoted = true;
                } else if(c == sep) {
                    endField();
                } else if(c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
                    endRow();
                } else {
                    field += c;
                }
            }
        }
        if(!stopped && (!field.isEmpty() || !fields.isEmpty() || fieldQuoted))
            endRow();
        if(decode.hasError()) {
            if(error)
                *error = QStringLiteral("%1 isn't valid text in the chosen encoding").arg(file);
            return false;
        }
        if(!haveColumns && onColumns)
            onColumns({});
        return true;
    };
}

/* ---- zip, for .xlsx -------------------------------------------------- */

namespace {

quint32 le32(const QByteArray &d, qsizetype at)
{
    if(at < 0 || at + 4 > d.size())
        return 0;
    const auto *p = reinterpret_cast<const uchar *>(d.constData() + at);
    return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24;
}

quint16 le16(const QByteArray &d, qsizetype at)
{
    if(at < 0 || at + 2 > d.size())
        return 0;
    const auto *p = reinterpret_cast<const uchar *>(d.constData() + at);
    return quint16(p[0] | p[1] << 8);
}

class Zip
{
public:
    bool open(const QString &file, QString *error)
    {
        QFile f(file);
        if(!f.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Can't read %1: %2").arg(file, f.errorString());
            return false;
        }
        m_data = f.readAll();
        /* the end of central directory record, within the last 64 KiB */
        qsizetype end = -1;
        for(qsizetype i = m_data.size() - 22; i >= 0 && i >= m_data.size() - 22 - 65535; --i)
            if(le32(m_data, i) == 0x06054b50) {
                end = i;
                break;
            }
        if(end < 0) {
            *error = QStringLiteral("%1 isn't an Excel workbook (.xlsx)").arg(file);
            return false;
        }
        const int count = le16(m_data, end + 10);
        qsizetype at = le32(m_data, end + 16);
        for(int n = 0; n < count; ++n) {
            if(le32(m_data, at) != 0x02014b50) {
                *error = QStringLiteral("%1 is damaged").arg(file);
                return false;
            }
            Entry e;
            e.method = le16(m_data, at + 10);
            e.compressed = le32(m_data, at + 20);
            e.size = le32(m_data, at + 24);
            const int nameLength = le16(m_data, at + 28);
            const int extra = le16(m_data, at + 30), comment = le16(m_data, at + 32);
            e.local = le32(m_data, at + 42);
            m_entries.insert(QString::fromUtf8(m_data.mid(at + 46, nameLength)), e);
            at += 46 + nameLength + extra + comment;
        }
        return true;
    }

    bool contains(const QString &name) const
    {
        return m_entries.contains(name);
    }

    QByteArray read(const QString &name, QString *error) const
    {
        const auto it = m_entries.constFind(name);
        if(it == m_entries.constEnd()) {
            *error = QStringLiteral("the workbook has no %1").arg(name);
            return {};
        }
        const Entry &e = *it;
        if(le32(m_data, e.local) != 0x04034b50) {
            *error = QStringLiteral("the workbook is damaged");
            return {};
        }
        const qsizetype start =
            e.local + 30 + le16(m_data, e.local + 26) + le16(m_data, e.local + 28);
        const QByteArray packed = m_data.mid(start, e.compressed);
        if(e.method == 0)
            return packed;
        if(e.method != 8) {
            *error = QStringLiteral("%1 is compressed in a way this can't read").arg(name);
            return {};
        }
        QByteArray out(qsizetype(e.size), Qt::Uninitialized);
        z_stream z{};
        if(inflateInit2(&z, -MAX_WBITS) != Z_OK) {
            *error = QStringLiteral("can't unpack the workbook");
            return {};
        }
        z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(packed.constData()));
        z.avail_in = uInt(packed.size());
        z.next_out = reinterpret_cast<Bytef *>(out.data());
        z.avail_out = uInt(out.size());
        const int rc = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        if(rc != Z_STREAM_END) {
            *error = QStringLiteral("the workbook is damaged (%1)").arg(name);
            return {};
        }
        out.resize(qsizetype(z.total_out));
        return out;
    }

private:
    struct Entry
    {
        quint16 method = 0;
        quint32 compressed = 0, size = 0, local = 0;
    };
    QByteArray m_data;
    QHash<QString, Entry> m_entries;
};

/* the text of the element the reader is on, all of its <t> runs */
QString runsText(QXmlStreamReader &x)
{
    QString text;
    const QStringView outer = x.name();
    const QString end = outer.toString();
    while(!x.atEnd()) {
        x.readNext();
        if(x.isStartElement() && x.name() == QLatin1String("t"))
            text += x.readElementText(QXmlStreamReader::IncludeChildElements);
        else if(x.isStartElement() && x.name() == QLatin1String("rPh"))
            x.skipCurrentElement(); /* phonetic hints, not the text */
        else if(x.isEndElement() && x.name() == end)
            break;
    }
    return text;
}

struct Workbook
{
    QStringList sheets;            /* in order */
    QHash<QString, QString> paths; /* sheet name -> zip entry */
    bool date1904 = false;
};

bool readWorkbook(const Zip &zip, Workbook *wb, QString *error)
{
    const QByteArray workbook = zip.read(QStringLiteral("xl/workbook.xml"), error);
    if(workbook.isEmpty())
        return false;
    QHash<QString, QString> ids; /* relationship id -> sheet name */
    QXmlStreamReader x(workbook);
    while(!x.atEnd()) {
        x.readNext();
        if(!x.isStartElement())
            continue;
        if(x.name() == QLatin1String("workbookPr")) {
            const QStringView d = x.attributes().value(QLatin1String("date1904"));
            wb->date1904 = d == QLatin1String("1") || d == QLatin1String("true");
        } else if(x.name() == QLatin1String("sheet")) {
            QString name, rid;
            for(const QXmlStreamAttribute &a : x.attributes()) {
                if(a.name() == QLatin1String("name"))
                    name = a.value().toString();
                else if(a.name() == QLatin1String("id"))
                    rid = a.value().toString();
            }
            wb->sheets << name;
            ids.insert(rid, name);
        }
    }
    const QByteArray rels = zip.read(QStringLiteral("xl/_rels/workbook.xml.rels"), error);
    QXmlStreamReader r(rels);
    while(!r.atEnd()) {
        r.readNext();
        if(!r.isStartElement() || r.name() != QLatin1String("Relationship"))
            continue;
        const QString id = r.attributes().value(QLatin1String("Id")).toString();
        QString target = r.attributes().value(QLatin1String("Target")).toString();
        if(!ids.contains(id))
            continue;
        target =
            target.startsWith(QLatin1Char('/')) ? target.mid(1) : QStringLiteral("xl/") + target;
        wb->paths.insert(ids.value(id), target);
    }
    if(wb->sheets.isEmpty()) {
        *error = QStringLiteral("the workbook has no sheets");
        return false;
    }
    return true;
}

/* which cell styles show a number as a date (1), a date and time (2) or a
 * time (3) */
QList<int> dateStyles(const Zip &zip)
{
    QString error;
    if(!zip.contains(QStringLiteral("xl/styles.xml")))
        return {};
    const QByteArray styles = zip.read(QStringLiteral("xl/styles.xml"), &error);
    const auto kindOf = [](int id, const QString &code) {
        if((id >= 14 && id <= 17) || (id >= 27 && id <= 31) || (id >= 34 && id <= 36) ||
           (id >= 50 && id <= 58))
            return 1;
        if(id == 22)
            return 2;
        if((id >= 18 && id <= 21) || (id >= 45 && id <= 47) || id == 32 || id == 33)
            return 3;
        if(code.isEmpty())
            return 0;
        /* a custom format: what's left once quoted text, escapes and
         * [colour]/[$-locale] parts are out */
        QString c = code;
        c.remove(QRegularExpression(QStringLiteral("\"[^\"]*\"|\\\\.|\\[[^\\]]*\\]|_.|\\*.")));
        c = c.toLower();
        const bool date = c.contains(QLatin1Char('y')) || c.contains(QLatin1Char('d'));
        const bool time = c.contains(QLatin1Char('h')) || c.contains(QLatin1Char('s'));
        return date && time ? 2 : date ? 1 : time ? 3 : 0;
    };
    QHash<int, QString> custom;
    QList<int> kinds;
    QXmlStreamReader x(styles);
    bool inCellXfs = false;
    while(!x.atEnd()) {
        x.readNext();
        if(x.isStartElement() && x.name() == QLatin1String("numFmt")) {
            custom.insert(x.attributes().value(QLatin1String("numFmtId")).toInt(),
                          x.attributes().value(QLatin1String("formatCode")).toString());
        } else if(x.isStartElement() && x.name() == QLatin1String("cellXfs")) {
            inCellXfs = true;
        } else if(x.isEndElement() && x.name() == QLatin1String("cellXfs")) {
            inCellXfs = false;
        } else if(inCellXfs && x.isStartElement() && x.name() == QLatin1String("xf")) {
            const int id = x.attributes().value(QLatin1String("numFmtId")).toInt();
            kinds << kindOf(id, custom.value(id));
        }
    }
    return kinds;
}

/* "AB12" -> 27 (zero-based column) */
int columnOf(QStringView ref)
{
    int col = 0;
    for(const QChar c : ref) {
        if(!c.isLetter())
            break;
        col = col * 26 + (c.toUpper().unicode() - 'A' + 1);
    }
    return col - 1;
}

QString excelDate(double serial, int kind, bool date1904)
{
    const QDate base = date1904 ? QDate(1904, 1, 1) : QDate(1899, 12, 30);
    const qint64 days = qint64(std::floor(serial));
    const qint64 ms = qint64(std::llround((serial - double(days)) * 86400000.0));
    const QDateTime t = QDateTime(base.addDays(days), QTime(0, 0), QTimeZone::UTC).addMSecs(ms);
    if(kind == 1 && ms == 0)
        return t.date().toString(Qt::ISODate);
    if(kind == 3 && days == 0)
        return t.time().toString(QStringLiteral("HH:mm:ss"));
    return t.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString excelNumber(const QString &v)
{
    /* Excel writes 17 significant digits: 0.1 as 0.10000000000000001 */
    if(!v.contains(QLatin1Char('.')) && !v.contains(QLatin1Char('E')) &&
       !v.contains(QLatin1Char('e')))
        return v;
    bool ok = false;
    const double d = v.toDouble(&ok);
    return ok ? QString::number(d, 'g', QLocale::FloatingPointShortest) : v;
}

} // namespace

QStringList xlsxSheets(const QString &file, QString *error)
{
    Zip zip;
    Workbook wb;
    if(!zip.open(file, error) || !readWorkbook(zip, &wb, error))
        return {};
    return wb.sheets;
}

Reader xlsxReader(const QString &file, const QString &sheet, bool header, bool emptyIsNull)
{
    return [file, sheet, header, emptyIsNull](const OnColumns &onColumns, const EachRow &each,
                                              QString *error) {
        Zip zip;
        Workbook wb;
        if(!zip.open(file, error) || !readWorkbook(zip, &wb, error))
            return false;
        const QString name = sheet.isEmpty() ? wb.sheets.first() : sheet;
        if(!wb.paths.contains(name)) {
            *error = QStringLiteral("the workbook has no sheet %1").arg(name);
            return false;
        }
        QStringList shared;
        if(zip.contains(QStringLiteral("xl/sharedStrings.xml"))) {
            const QByteArray s = zip.read(QStringLiteral("xl/sharedStrings.xml"), error);
            QXmlStreamReader x(s);
            while(!x.atEnd()) {
                x.readNext();
                if(x.isStartElement() && x.name() == QLatin1String("si"))
                    shared << runsText(x);
            }
        }
        const QList<int> styles = dateStyles(zip);
        const QByteArray xml = zip.read(wb.paths.value(name), error);
        if(xml.isEmpty())
            return false;

        /* how wide: the widest row */
        int width = 0;
        {
            QXmlStreamReader x(xml);
            int next = 0;
            while(!x.atEnd()) {
                x.readNext();
                if(x.isStartElement() && x.name() == QLatin1String("row"))
                    next = 0;
                else if(x.isStartElement() && x.name() == QLatin1String("c")) {
                    const QStringView ref = x.attributes().value(QLatin1String("r"));
                    const int col = ref.isEmpty() ? next : columnOf(ref);
                    width = std::max(width, col + 1);
                    next = col + 1;
                }
            }
        }

        QXmlStreamReader x(xml);
        QStringList cells;
        QList<bool> filled;
        bool inRow = false, haveColumns = false;
        int next = 0;
        while(!x.atEnd()) {
            x.readNext();
            if(x.isStartElement() && x.name() == QLatin1String("row")) {
                inRow = true;
                next = 0;
                cells = QStringList(width);
                filled = QList<bool>(width, false);
            } else if(inRow && x.isStartElement() && x.name() == QLatin1String("c")) {
                const QXmlStreamAttributes a = x.attributes();
                const QStringView ref = a.value(QLatin1String("r"));
                const int col = ref.isEmpty() ? next : columnOf(ref);
                next = col + 1;
                const QString type = a.value(QLatin1String("t")).toString();
                const int style = a.value(QLatin1String("s")).toInt();
                QString value;
                bool has = false;
                while(!x.atEnd()) {
                    x.readNext();
                    if(x.isStartElement() && x.name() == QLatin1String("v")) {
                        value = x.readElementText();
                        has = true;
                    } else if(x.isStartElement() && x.name() == QLatin1String("is")) {
                        value = runsText(x);
                        has = true;
                    } else if(x.isEndElement() && x.name() == QLatin1String("c")) {
                        break;
                    }
                }
                if(!has || col < 0 || col >= width)
                    continue;
                if(type == QLatin1String("s"))
                    value = shared.value(value.toInt());
                else if(type == QLatin1String("b"))
                    value = value == QLatin1String("1") ? QStringLiteral("true")
                                                        : QStringLiteral("false");
                else if(type.isEmpty() || type == QLatin1String("n")) {
                    const int kind = styles.value(style);
                    bool ok = false;
                    const double d = value.toDouble(&ok);
                    value = kind && ok ? excelDate(d, kind, wb.date1904) : excelNumber(value);
                }
                cells[col] = value;
                filled[col] = true;
            } else if(inRow && x.isEndElement() && x.name() == QLatin1String("row")) {
                inRow = false;
                if(!filled.contains(true))
                    continue; /* an empty row */
                if(!haveColumns) {
                    haveColumns = true;
                    if(onColumns)
                        onColumns(header ? columnNames(cells) : numbered(width));
                    if(header)
                        continue;
                }
                Row row;
                row.values.resize(width);
                row.nulls.resize(width);
                for(int i = 0; i < width; ++i) {
                    row.values[i] = cells[i].toUtf8();
                    row.nulls[i] = !filled[i] || (emptyIsNull && cells[i].isEmpty());
                }
                if(!each(row))
                    return true;
            }
        }
        if(x.hasError()) {
            *error = QStringLiteral("the sheet is damaged: %1").arg(x.errorString());
            return false;
        }
        if(!haveColumns && onColumns)
            onColumns({});
        return true;
    };
}

/* ---- another database ------------------------------------------------ */

bool readConnection(IDbConnection *c, const QString &database, const QString &table,
                    const QString &query, const OnColumns &onColumns, const EachRow &each,
                    QString *error)
{
    const QString sql = query.trimmed().isEmpty()
                            ? QStringLiteral("SELECT * FROM %1").arg(c->qualify(database, table))
                            : query.trimmed();
    Row row;
    QString err;
    const bool ok = c->streamQuery(
        sql, &err,
        [&](const QStringList &headers) {
            if(onColumns)
                onColumns(headers);
        },
        [&](const QVector<QByteArray> &fields, const QVector<bool> &isNull) {
            row.values = fields;
            row.nulls = isNull;
            return each(row);
        });
    if(!ok && error)
        *error = err;
    return ok;
}

/* ---- column types ------------------------------------------------------ */

void TypeGuess::add(const QByteArray &value, bool isNull)
{
    if(isNull)
        return;
    m_seen = true;
    const QString s = QString::fromUtf8(value);
    m_maxLength = std::max(m_maxLength, int(s.size()));
    const QString t = s.trimmed();
    if(t.isEmpty()) {
        m_int = m_big = m_decimal = m_double = m_bool = m_date = m_dateTime = false;
        return;
    }
    static const QRegularExpression integer(QStringLiteral("^[+-]?(\\d+)$"));
    static const QRegularExpression decimal(QStringLiteral("^[+-]?(\\d+)\\.(\\d+)$"));
    static const QRegularExpression floating(
        QStringLiteral("^[+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?$"));
    static const QRegularExpression date(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
    static const QRegularExpression dateTime(
        QStringLiteral("^(\\d{4}-\\d{2}-\\d{2})([ T](\\d{2}:\\d{2}(:\\d{2}(\\.\\d{1,6})?)?))?$"));
    /* a leading zero (a postcode, a phone number) is text, not a number */
    const auto leadingZero = [](const QString &digits) {
        return digits.size() > 1 && digits.startsWith(QLatin1Char('0'));
    };
    const auto whole = integer.match(t);
    if(whole.hasMatch() && leadingZero(whole.captured(1))) {
        m_int = m_big = m_decimal = m_double = false;
    } else if(whole.hasMatch()) {
        bool ok = false;
        const qint64 n = t.toLongLong(&ok);
        if(!ok)
            m_int = m_big = false;
        else if(n < std::numeric_limits<qint32>::min() || n > std::numeric_limits<qint32>::max())
            m_int = false;
        m_intDigits = std::max(m_intDigits, int(whole.captured(1).size()));
    } else {
        m_int = m_big = false;
        if(const auto d = decimal.match(t); d.hasMatch() && !leadingZero(d.captured(1))) {
            m_intDigits = std::max(m_intDigits, int(d.captured(1).size()));
            m_fraction = std::max(m_fraction, int(d.captured(2).size()));
        } else {
            m_decimal = false;
        }
        if(!floating.match(t).hasMatch())
            m_double = false;
    }
    if(t.compare(QLatin1String("true"), Qt::CaseInsensitive) != 0 &&
       t.compare(QLatin1String("false"), Qt::CaseInsensitive) != 0)
        m_bool = false;
    if(!date.match(t).hasMatch() || !QDate::fromString(t, Qt::ISODate).isValid())
        m_date = false;
    const auto dt = dateTime.match(t);
    if(!dt.hasMatch() || !QDate::fromString(dt.captured(1), Qt::ISODate).isValid())
        m_dateTime = false;
}

ColumnType TypeGuess::type() const
{
    ColumnType t;
    if(!m_seen)
        return t; /* nothing but NULLs: text */
    if(m_int) {
        t.kind = ColumnType::Integer;
    } else if(m_big) {
        t.kind = ColumnType::BigInt;
    } else if(m_decimal && m_intDigits + m_fraction <= 38) {
        t.kind = ColumnType::Decimal;
        t.scale = m_fraction;
        /* a few more whole digits than seen, for rows the guess didn't see */
        t.precision = std::min(38, m_intDigits + m_fraction + 4);
    } else if(m_double) {
        t.kind = ColumnType::Double;
    } else if(m_bool) {
        t.kind = ColumnType::Boolean;
    } else if(m_date) {
        t.kind = ColumnType::Date;
    } else if(m_dateTime) {
        t.kind = ColumnType::DateTime;
    } else if(m_maxLength > 255) {
        t.kind = ColumnType::LongText;
    }
    return t;
}

ColumnType fromSqlType(const QString &sqlType)
{
    const QString s = sqlType.trimmed().toLower();
    static const QRegularExpression args(
        QStringLiteral("\\(\\s*(\\d+)\\s*(?:,\\s*(\\d+)\\s*)?\\)"));
    const auto m = args.match(s);
    ColumnType t;
    if(s.startsWith(QLatin1String("tinyint(1)")) || s.startsWith(QLatin1String("bool")))
        t.kind = ColumnType::Boolean;
    else if(s.contains(QLatin1String("bigint")) || s.contains(QLatin1String("bigserial")) ||
            s == QLatin1String("int8"))
        t.kind = ColumnType::BigInt;
    else if(s.contains(QLatin1String("int")) || s.contains(QLatin1String("serial")))
        t.kind = ColumnType::Integer;
    else if(s.startsWith(QLatin1String("decimal")) || s.startsWith(QLatin1String("numeric"))) {
        if(m.hasMatch()) {
            t.kind = ColumnType::Decimal;
            t.precision = m.captured(1).toInt();
            t.scale = m.captured(2).toInt();
        } else {
            t.kind = ColumnType::Double;
        }
    } else if(s.contains(QLatin1String("double")) || s.contains(QLatin1String("float")) ||
              s.contains(QLatin1String("real")))
        t.kind = ColumnType::Double;
    else if(s.startsWith(QLatin1String("datetime")) || s.startsWith(QLatin1String("timestamp")))
        t.kind = ColumnType::DateTime;
    else if(s == QLatin1String("date"))
        t.kind = ColumnType::Date;
    else if(s.contains(QLatin1String("blob")) || s.contains(QLatin1String("binary")) ||
            s == QLatin1String("bytea"))
        t.kind = ColumnType::Binary;
    else if((s.startsWith(QLatin1String("varchar")) || s.startsWith(QLatin1String("char")) ||
             s.startsWith(QLatin1String("character"))) &&
            m.hasMatch() && m.captured(1).toInt() <= 1000)
        t.length = std::max(1, m.captured(1).toInt());
    else if(s.startsWith(QLatin1String("time")))
        t.length = 32;
    else
        t.kind = ColumnType::LongText;
    return t;
}

QString sqlType(const ColumnType &t, SqlDriverType driver)
{
    const bool my = driver == SqlDriverType::Mysql, pg = driver == SqlDriverType::Postgres;
    switch(t.kind) {
        case ColumnType::Integer:
            return my   ? QStringLiteral("INT")
                   : pg ? QStringLiteral("integer")
                        : QStringLiteral("INTEGER");
        case ColumnType::BigInt:
            return my   ? QStringLiteral("BIGINT")
                   : pg ? QStringLiteral("bigint")
                        : QStringLiteral("INTEGER");
        case ColumnType::Decimal:
            return my   ? QStringLiteral("DECIMAL(%1,%2)").arg(t.precision).arg(t.scale)
                   : pg ? QStringLiteral("numeric(%1,%2)").arg(t.precision).arg(t.scale)
                        : QStringLiteral("NUMERIC");
        case ColumnType::Double:
            return my   ? QStringLiteral("DOUBLE")
                   : pg ? QStringLiteral("double precision")
                        : QStringLiteral("REAL");
        case ColumnType::Boolean:
            return my   ? QStringLiteral("TINYINT(1)")
                   : pg ? QStringLiteral("boolean")
                        : QStringLiteral("INTEGER");
        case ColumnType::Date:
            return pg ? QStringLiteral("date") : QStringLiteral("DATE");
        case ColumnType::DateTime:
            return my   ? QStringLiteral("DATETIME")
                   : pg ? QStringLiteral("timestamp")
                        : QStringLiteral("DATETIME");
        case ColumnType::Text:
            return my   ? QStringLiteral("VARCHAR(%1)").arg(t.length)
                   : pg ? QStringLiteral("varchar(%1)").arg(t.length)
                        : QStringLiteral("TEXT");
        case ColumnType::LongText:
            return my   ? QStringLiteral("LONGTEXT")
                   : pg ? QStringLiteral("text")
                        : QStringLiteral("TEXT");
        case ColumnType::Binary:
            return my   ? QStringLiteral("LONGBLOB")
                   : pg ? QStringLiteral("bytea")
                        : QStringLiteral("BLOB");
    }
    return QStringLiteral("TEXT");
}

QString describe(const ColumnType &t)
{
    switch(t.kind) {
        case ColumnType::Integer:
            return QStringLiteral("whole number");
        case ColumnType::BigInt:
            return QStringLiteral("big whole number");
        case ColumnType::Decimal:
            return QStringLiteral("decimal (%1 digits, %2 after the point)")
                .arg(t.precision)
                .arg(t.scale);
        case ColumnType::Double:
            return QStringLiteral("floating point");
        case ColumnType::Boolean:
            return QStringLiteral("true / false");
        case ColumnType::Date:
            return QStringLiteral("date");
        case ColumnType::DateTime:
            return QStringLiteral("date and time");
        case ColumnType::Text:
            return QStringLiteral("text (up to %1)").arg(t.length);
        case ColumnType::LongText:
            return QStringLiteral("long text");
        case ColumnType::Binary:
            return QStringLiteral("binary");
    }
    return {};
}

/* ---- the import ---------------------------------------------------------- */

QString identifier(const QString &name, const QString &fallback)
{
    QString out;
    /* letters with their combining marks (a Bengali or Hindi vowel sign
     * isn't a letter of its own), digits and _ */
    for(const QChar c : name.trimmed())
        out += c.isLetterOrNumber() || c.isMark() || c == QLatin1Char('_') ? c : QLatin1Char('_');
    out.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    while(out.startsWith(QLatin1Char('_')))
        out.remove(0, 1);
    while(out.endsWith(QLatin1Char('_')))
        out.chop(1);
    if(out.isEmpty())
        return fallback;
    if(out[0].isDigit())
        out.prepend(QLatin1Char('_'));
    return out.left(64);
}

QString createTableSql(IDbConnection *target, const Plan &plan)
{
    QStringList columns;
    for(const Mapping &m : plan.columns)
        columns << QStringLiteral("  %1 %2").arg(target->quoteIdent(m.target),
                                                 m.declared.trimmed().isEmpty()
                                                     ? sqlType(m.type, target->driverType())
                                                     : m.declared.trimmed());
    return QStringLiteral("CREATE TABLE %1 (\n%2\n)")
        .arg(target->qualify(plan.database, plan.table), columns.join(QStringLiteral(",\n")));
}

bool run(IDbConnection *target, const Plan &plan, const Reader &source, qint64 *rows,
         QString *error, const std::function<void(qint64)> &progress,
         const std::function<bool()> &cancelled)
{
    if(rows)
        *rows = 0;
    if(plan.columns.isEmpty()) {
        if(error)
            *error = QStringLiteral("No columns to import: pick at least one.");
        return false;
    }
    QString err;
    if(plan.createTable && !target->query(createTableSql(target, plan), nullptr, &err)) {
        if(error)
            *error = QStringLiteral("Can't create the table: %1").arg(err);
        return false;
    }
    const auto undo = [&] {
        target->query(QStringLiteral("ROLLBACK"), nullptr, nullptr);
        if(plan.createTable)
            target->query(
                QStringLiteral("DROP TABLE %1").arg(target->qualify(plan.database, plan.table)),
                nullptr, nullptr);
    };
    target->query(target->driverType() == SqlDriverType::Mysql ? QStringLiteral("START TRANSACTION")
                                                               : QStringLiteral("BEGIN"),
                  nullptr, nullptr);
    if(plan.emptyFirst && !plan.createTable &&
       !target->query(
           QStringLiteral("DELETE FROM %1").arg(target->qualify(plan.database, plan.table)),
           nullptr, &err)) {
        undo();
        if(error)
            *error = QStringLiteral("Can't empty the table: %1").arg(err);
        return false;
    }

    QStringList names;
    for(const Mapping &m : plan.columns)
        names << target->quoteIdent(m.target);
    const QByteArray head =
        QStringLiteral("INSERT INTO %1 (%2) VALUES ")
            .arg(target->qualify(plan.database, plan.table), names.join(QStringLiteral(", ")))
            .toUtf8();
    QByteArray batch;
    int pending = 0;
    qint64 done = 0, first = 1;
    bool failed = false, stopped = false;
    int columnCount = -1;
    const auto flush = [&] {
        if(!pending)
            return true;
        if(!target->query(QString::fromUtf8(head + batch), nullptr, &err)) {
            failed = true;
            err = QStringLiteral("rows %1–%2: %3").arg(first).arg(done).arg(err);
            return false;
        }
        batch.clear();
        pending = 0;
        first = done + 1;
        if(progress)
            progress(done);
        if(cancelled && cancelled()) {
            stopped = true;
            return false;
        }
        return true;
    };
    const auto boolean = [](const QByteArray &v) -> QByteArray {
        const QByteArray l = v.trimmed().toLower();
        if(l == "true" || l == "yes" || l == "y" || l == "t" || l == "1")
            return "1";
        if(l == "false" || l == "no" || l == "n" || l == "f" || l == "0")
            return "0";
        return v;
    };
    QString readErr;
    const bool read = source(
        [&](const QStringList &columns) {
            columnCount = int(columns.size());
            for(const Mapping &m : plan.columns)
                if(m.source >= columnCount && !failed) {
                    failed = true;
                    err = QStringLiteral("the source has no column %1").arg(m.source + 1);
                }
        },
        [&](const Row &row) {
            if(failed)
                return false;
            QByteArray tuple("(");
            for(int i = 0; i < plan.columns.size(); ++i) {
                const Mapping &m = plan.columns[i];
                const bool isNull = m.source >= row.nulls.size() || row.nulls[m.source];
                QByteArray v = m.source < row.values.size() ? row.values[m.source] : QByteArray();
                if(plan.createTable && m.type.kind == ColumnType::Boolean && !isNull)
                    v = boolean(v);
                tuple += (i ? ", " : "") + SqlDump::literal(target, v, isNull, m.binary);
            }
            batch += (pending ? ",\n" : "") + tuple + ')';
            ++pending;
            ++done;
            return pending < plan.rowsPerInsert || flush();
        },
        &readErr);
    if(!failed && !stopped && read)
        flush();
    if(!read || failed || stopped) {
        undo();
        if(error)
            *error = !read     ? readErr
                     : stopped ? QStringLiteral("Cancelled: nothing was imported.")
                               : QStringLiteral("Nothing was imported: %1").arg(err);
        return false;
    }
    if(!target->query(QStringLiteral("COMMIT"), nullptr, &err)) {
        undo();
        if(error)
            *error = QStringLiteral("Nothing was imported: %1").arg(err);
        return false;
    }
    if(rows)
        *rows = done;
    return true;
}

} // namespace ExternalData
