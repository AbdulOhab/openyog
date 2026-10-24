/* OpenYog — Tools ▸ Import External Data: what it reads and how it writes.
 *
 * Three kinds of source, each read as columns and then rows (streamed: a
 * big file never has to fit in memory at once, apart from an .xlsx sheet's
 * own XML):
 *   - a CSV file: the separator found from the file when not given, quotes
 *     with doubled-quote escaping, fields running over line ends, an
 *     optional header row, and an empty unquoted field read as NULL (a
 *     quoted "" stays an empty string);
 *   - an Excel workbook (.xlsx), one sheet of it: shared and inline strings,
 *     numbers in their shortest exact form, booleans, and cells formatted as
 *     dates or times read as ISO dates (1900 and 1904 date systems);
 *   - another SQL connection: a table or a query, as bytes, binary columns
 *     flagged so they're written as hex literals.
 *
 * The rows go into a table of the target connection, an existing one or one
 * made for them (CREATE TABLE with a type per column: guessed from the data
 * for a file, carried over from the source's columns for a table), mapped
 * column by column, a batch of rows per INSERT, all in one transaction so a
 * failure leaves the table as it was (and drops a table made for it). */
#pragma once

#include "ConnectionParams.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class IDbConnection;

namespace ExternalData {

struct Row
{
    QVector<QByteArray> values; /* UTF-8 text, or raw bytes from a database */
    QVector<bool> nulls;
};
using OnColumns = std::function<void(const QStringList &columns)>;
using EachRow = std::function<bool(const Row &row)>; /* false: stop reading */
/* reads a source: its columns once, then its rows; false + *error on failure
 * (stopping early through `each` isn't a failure) */
using Reader = std::function<bool(const OnColumns &onColumns, const EachRow &each, QString *error)>;

/* ---- CSV ------------------------------------------------------------- */
struct CsvOptions
{
    QChar separator;    /* null: found from the file */
    QChar quote = u'"'; /* null: no quoting */
    bool header = true;
    bool emptyIsNull = true;
    enum Encoding { Utf8, Utf16, Latin1 } encoding = Utf8;
};
/* the likeliest separator among , ; tab | in the start of a file */
QChar detectSeparator(const QString &sample, QChar quote = u'"');
Reader csvReader(const QString &file, const CsvOptions &options);

/* ---- Excel (.xlsx) ----------------------------------------------------- */
QStringList xlsxSheets(const QString &file, QString *error);
Reader xlsxReader(const QString &file, const QString &sheet, bool header, bool emptyIsNull = true);

/* ---- another database ------------------------------------------------ */
/* a table (`query` empty) or a query of an open connection, as bytes; the
 * Import External Data dialog opens a connection of its own for it */
bool readConnection(IDbConnection *c, const QString &database, const QString &table,
                    const QString &query, const OnColumns &onColumns, const EachRow &each,
                    QString *error);

/* ---- column types ------------------------------------------------------ */
struct ColumnType
{
    enum Kind { Integer, BigInt, Decimal, Double, Boolean, Date, DateTime, Text, LongText, Binary };
    Kind kind = Text;
    int length = 255;  /* Text: VARCHAR(length) */
    int precision = 0; /* Decimal */
    int scale = 0;
};
/* the type a column's values fit, value by value */
class TypeGuess
{
public:
    void add(const QByteArray &value, bool isNull);
    ColumnType type() const;

private:
    bool m_seen = false;
    bool m_int = true, m_big = true, m_decimal = true, m_double = true, m_bool = true,
         m_date = true, m_dateTime = true;
    int m_intDigits = 1, m_fraction = 0, m_maxLength = 0;
};
/* a source database column's declared type, as the nearest of these */
ColumnType fromSqlType(const QString &sqlType);
/* how `driver` spells it */
QString sqlType(const ColumnType &type, SqlDriverType driver);
QString describe(const ColumnType &type); /* for the wizard: "whole number" … */

/* ---- the import ---------------------------------------------------------- */
struct Mapping
{
    int source = 0;      /* the source column's index */
    QString target;      /* the target column */
    ColumnType type;     /* a new table's column type; Boolean values are written 1/0 */
    QString declared;    /* the type as typed in the wizard, used instead of `type`'s */
    bool binary = false; /* raw bytes from a database: a hex literal */
};
struct Plan
{
    QString database, table;
    bool createTable = false; /* make the table from the mappings first */
    bool emptyFirst = false;  /* an existing table: its rows out first */
    QList<Mapping> columns;
    int rowsPerInsert = 100;
};
/* the CREATE TABLE a plan with createTable makes */
QString createTableSql(IDbConnection *target, const Plan &plan);
/* reads `source` into the table; *rows says how many went in. Progress is
 * reported after every batch; `cancelled` is asked between batches and a
 * cancelled import is rolled back like a failed one. */
bool run(IDbConnection *target, const Plan &plan, const Reader &source, qint64 *rows,
         QString *error, const std::function<void(qint64 rows)> &progress = {},
         const std::function<bool()> &cancelled = {});

/* a name usable as a table or column name: what's left of `name` after the
 * characters that would need quoting, or `fallback` */
QString identifier(const QString &name, const QString &fallback);

} // namespace ExternalData
