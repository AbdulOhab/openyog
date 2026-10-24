/* OpenYog — unit tests for qt/ExternalData: reading CSV files and Excel
 * workbooks, guessing column types, and importing into real SQLite
 * databases (from a file, and from another database). */
#include "ExternalData.h"
#include "db/SqliteConnection.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <sqlite3.h>

#include <memory>

using namespace ExternalData;

namespace {

const QString kFixtures = QStringLiteral(FIXTURES);

struct Read
{
    QStringList columns;
    QList<QStringList> rows; /* NULL as "∅" */
    QString error;
    bool ok = false;
};

Read readAll(const Reader &reader, int stopAfter = -1)
{
    Read r;
    r.ok = reader([&](const QStringList &c) { r.columns = c; },
                  [&](const Row &row) {
                      QStringList cells;
                      for(int i = 0; i < row.values.size(); ++i)
                          cells << (row.nulls[i] ? QStringLiteral("∅")
                                                 : QString::fromUtf8(row.values[i]));
                      r.rows << cells;
                      return stopAfter < 0 || r.rows.size() < stopAfter;
                  },
                  &r.error);
    return r;
}

std::unique_ptr<SqliteConnection> open(const QString &file)
{
    sqlite3 *db = nullptr;
    sqlite3_open(file.toUtf8().constData(), &db);
    return std::make_unique<SqliteConnection>(db);
}

QList<QStringList> rows(IDbConnection *c, const QString &sql)
{
    DbResultSet rs;
    c->query(sql, &rs, nullptr);
    return rs.rows;
}

Plan planFor(const QString &table, const QStringList &columns, const QList<ColumnType> &types)
{
    Plan p;
    p.database = QStringLiteral("main");
    p.table = table;
    p.createTable = true;
    for(int i = 0; i < columns.size(); ++i)
        p.columns.append({i, columns[i], types.value(i)});
    return p;
}

QList<ColumnType> guess(const Reader &reader)
{
    QList<TypeGuess> g;
    reader([&](const QStringList &c) { g.resize(c.size()); },
           [&](const Row &row) {
               for(int i = 0; i < row.values.size() && i < g.size(); ++i)
                   g[i].add(row.values[i], row.nulls[i]);
               return true;
           },
           nullptr);
    QList<ColumnType> types;
    for(const TypeGuess &t : g)
        types << t.type();
    return types;
}

} // namespace

class TestExternalData : public QObject
{
    Q_OBJECT
private slots:
    void separators()
    {
        QCOMPARE(detectSeparator(QStringLiteral("a,b,c\n1,2,3\n")), QChar(','));
        QCOMPARE(detectSeparator(QStringLiteral("a;b;c\n1;\"2,5\";3\n")), QChar(';'));
        QCOMPARE(detectSeparator(QStringLiteral("a\tb\n1\t2\n")), QChar('\t'));
        QCOMPARE(detectSeparator(QStringLiteral("a|b|c\n1|2|3")), QChar('|'));
        /* the separator inside quotes doesn't count */
        QCOMPARE(detectSeparator(QStringLiteral("name;note\nx;\"a,b,c,d\"\n")), QChar(';'));
        QCOMPARE(detectSeparator(QStringLiteral("just one column\n")), QChar(','));
    }

    void csv()
    {
        CsvOptions o; /* separator found: ; */
        const Read r = readAll(csvReader(kFixtures + "/import_people.csv", o));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.columns,
                 QStringList({"id", "name", "salary", "joined", "active", "code", "note"}));
        QCOMPARE(r.rows.size(), 3); /* the blank line isn't a row */
        QCOMPARE(r.rows[0],
                 QStringList({"1", "Alice", "5000.50", "2024-01-15", "true", "0101", "∅"}));
        QCOMPARE(r.rows[1], QStringList({"2", "রহিম", "4200", "2023-06-01 10:30:00", "false",
                                         "0202", "line one\r\nline two"}));
        /* a doubled quote, the separator inside quotes, "" an empty string */
        QCOMPARE(r.rows[2], QStringList({"3", "O\"Brien; Jr", "0.1", "∅", "TRUE", "0303", ""}));

        o.header = false;
        o.emptyIsNull = false;
        const Read raw = readAll(csvReader(kFixtures + "/import_people.csv", o));
        QCOMPARE(raw.columns.size(), 7);
        QCOMPARE(raw.columns.first(), QStringLiteral("column1"));
        QCOMPARE(raw.rows.size(), 4);
        QCOMPARE(raw.rows[1].last(), QString()); /* an empty field, NULL not wanted */

        /* stopping early isn't an error */
        const Read one = readAll(csvReader(kFixtures + "/import_people.csv", CsvOptions()), 1);
        QVERIFY(one.ok);
        QCOMPARE(one.rows.size(), 1);
    }

    void csvEdges()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("edges.csv");
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        /* a BOM, a header with blanks and a duplicate, short and long rows,
         * a quoted field running past the end of the file */
        f.write("\xEF\xBB\xBF"
                "a,,a,b\n1,2\n3,4,5,6,7\n\"open");
        f.close();
        const Read r = readAll(csvReader(file, CsvOptions()));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.columns, QStringList({"a", "column2", "a_2", "b"}));
        QCOMPARE(r.rows.size(), 3);
        QCOMPARE(r.rows[0], QStringList({"1", "2", "∅", "∅"}));
        QCOMPARE(r.rows[1], QStringList({"3", "4", "5", "6"}));
        QCOMPARE(r.rows[2], QStringList({"open", "∅", "∅", "∅"}));
        QVERIFY(!readAll(csvReader(dir.filePath("missing.csv"), CsvOptions())).ok);
    }

    void excel()
    {
        QString error;
        QCOMPARE(xlsxSheets(kFixtures + "/import_people.xlsx", &error),
                 QStringList({"People", "Other"}));
        const Read r = readAll(xlsxReader(kFixtures + "/import_people.xlsx", "People", true));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.columns, QStringList({"id", "name", "salary", "joined", "active", "note"}));
        QCOMPARE(r.rows.size(), 4);
        QCOMPARE(r.rows[0], QStringList({"1", "Alice", "5000.5", "2024-01-15", "true",
                                         "a, \"quoted\" note "}));
        /* a rich-text string's runs, without its phonetic hint; a date and
         * time format; an inline string */
        QCOMPARE(r.rows[1],
                 QStringList({"2", "রহিম", "4200", "2023-06-01 10:30:00", "false", "inline text"}));
        /* 17 digits read back as the number meant; missing cells are NULL */
        QCOMPARE(r.rows[2], QStringList({"3", "Dana", "0.1", "∅", "true", "∅"}));
        QCOMPARE(r.rows[3], QStringList({"4", "∅", "1500", "∅", "∅", "∅"}));

        const Read other = readAll(xlsxReader(kFixtures + "/import_people.xlsx", "Other", false));
        QCOMPARE(other.columns, QStringList({"column1"}));
        QCOMPARE(other.rows, QList<QStringList>({{"a"}, {"7"}}));

        QVERIFY(!readAll(xlsxReader(kFixtures + "/import_people.xlsx", "Nope", true)).ok);
        QVERIFY(xlsxSheets(kFixtures + "/import_people.csv", &error).isEmpty());
        QVERIFY(error.contains("isn't an Excel workbook"));
    }

    void typeGuesses()
    {
        const auto of = [](const QStringList &values) {
            TypeGuess g;
            for(const QString &v : values)
                g.add(v.toUtf8(), v == QStringLiteral("∅"));
            return g.type();
        };
        QCOMPARE(of({"1", "-20", "∅"}).kind, ColumnType::Integer);
        QCOMPARE(of({"1", "3000000000"}).kind, ColumnType::BigInt);
        const ColumnType money = of({"5000.50", "4200", "0.1"});
        QCOMPARE(money.kind, ColumnType::Decimal);
        QCOMPARE(money.scale, 2);
        QCOMPARE(money.precision, 10); /* 4 whole digits seen, 4 to spare */
        QCOMPARE(of({"1e3", "2.5"}).kind, ColumnType::Double);
        QCOMPARE(of({"true", "FALSE"}).kind, ColumnType::Boolean);
        QCOMPARE(of({"2024-01-15", "2024-02-29"}).kind, ColumnType::Date);
        QCOMPARE(of({"2024-01-15", "2024-01-15T10:30"}).kind, ColumnType::DateTime);
        QCOMPARE(of({"2024-02-30"}).kind, ColumnType::Text);   /* no such day */
        QCOMPARE(of({"0101", "0202"}).kind, ColumnType::Text); /* codes keep their zeros */
        QCOMPARE(of({"1", ""}).kind, ColumnType::Text);
        QCOMPARE(of({"∅"}).kind, ColumnType::Text);
        QCOMPARE(of({QString(300, QLatin1Char('x'))}).kind, ColumnType::LongText);

        QCOMPARE(fromSqlType("int(11)").kind, ColumnType::Integer);
        QCOMPARE(fromSqlType("bigint unsigned").kind, ColumnType::BigInt);
        QCOMPARE(fromSqlType("tinyint(1)").kind, ColumnType::Boolean);
        QCOMPARE(fromSqlType("boolean").kind, ColumnType::Boolean);
        QCOMPARE(fromSqlType("numeric(10,2)").precision, 10);
        QCOMPARE(fromSqlType("character varying(40)").length, 40);
        QCOMPARE(fromSqlType("timestamp without time zone").kind, ColumnType::DateTime);
        QCOMPARE(fromSqlType("bytea").kind, ColumnType::Binary);
        QCOMPARE(fromSqlType("json").kind, ColumnType::LongText);

        QCOMPARE(sqlType(money, SqlDriverType::Mysql), QStringLiteral("DECIMAL(10,2)"));
        QCOMPARE(sqlType(money, SqlDriverType::Postgres), QStringLiteral("numeric(10,2)"));
        QCOMPARE(sqlType({ColumnType::Boolean}, SqlDriverType::Postgres),
                 QStringLiteral("boolean"));
        QCOMPARE(sqlType({ColumnType::Text, 40}, SqlDriverType::Mysql),
                 QStringLiteral("VARCHAR(40)"));

        QCOMPARE(identifier("Order Date (UTC)", "x"), QStringLiteral("Order_Date_UTC"));
        QCOMPARE(identifier("2024 sales", "x"), QStringLiteral("_2024_sales"));
        QCOMPARE(identifier(" -- ", "fallback"), QStringLiteral("fallback"));
        QCOMPARE(identifier("নাম", "x"), QStringLiteral("নাম"));
    }

    void importIntoNewTables()
    {
        QTemporaryDir dir;
        const QString dbFile = dir.filePath("target.sqlite");
        auto c = open(dbFile);

        /* a CSV, with the types its data has */
        const Reader csv = csvReader(kFixtures + "/import_people.csv", CsvOptions());
        const QList<ColumnType> types = guess(csv);
        Plan p =
            planFor("people", {"id", "name", "salary", "joined", "active", "code", "note"}, types);
        QVERIFY(createTableSql(c.get(), p).contains("\"active\" INTEGER"));
        qint64 n = 0;
        QString error;
        QList<qint64> progress;
        p.rowsPerInsert = 2;
        QVERIFY2(run(c.get(), p, csv, &n, &error, [&](qint64 done) { progress << done; }),
                 qPrintable(error));
        QCOMPARE(n, qint64(3));
        QCOMPARE(progress, QList<qint64>({2, 3}));
        QCOMPARE(rows(c.get(), "SELECT id, name, active, code, note IS NULL, joined FROM people "
                               "ORDER BY id"),
                 QList<QStringList>({{"1", "Alice", "1", "0101", "1", "2024-01-15"},
                                     {"2", "রহিম", "0", "0202", "0", "2023-06-01 10:30:00"},
                                     {"3", "O\"Brien; Jr", "1", "0303", "0", "NULL"}}));

        /* a workbook */
        const Reader xlsx = xlsxReader(kFixtures + "/import_people.xlsx", "People", true);
        Plan q = planFor("from_excel", {"id", "name", "salary", "joined", "active", "note"},
                         guess(xlsx));
        QVERIFY2(run(c.get(), q, xlsx, &n, &error), qPrintable(error));
        QCOMPARE(n, qint64(4));
        QCOMPARE(rows(c.get(), "SELECT salary, joined FROM from_excel ORDER BY id"),
                 QList<QStringList>({{"5000.5", "2024-01-15"},
                                     {"4200", "2023-06-01 10:30:00"},
                                     {"0.1", "NULL"},
                                     {"1500", "NULL"}}));

        /* another database's table into this one, binary data included */
        const QString sourceFile = dir.filePath("source.sqlite");
        auto src = open(sourceFile);
        src->query("CREATE TABLE files (id INTEGER PRIMARY KEY, name TEXT, data BLOB)", nullptr,
                   nullptr);
        src->query("INSERT INTO files VALUES (1, 'a', x'00FF89'), (2, 'b', NULL)", nullptr,
                   nullptr);
        Plan f = planFor("files_copy", {"id", "name", "data"},
                         {fromSqlType("INTEGER"), fromSqlType("TEXT"), fromSqlType("BLOB")});
        f.columns[2].binary = true;
        const Reader fromDb = [&](const OnColumns &oc, const EachRow &each, QString *e) {
            return readConnection(src.get(), "main", "files", {}, oc, each, e);
        };
        QVERIFY2(run(c.get(), f, fromDb, &n, &error), qPrintable(error));
        QCOMPARE(
            rows(c.get(), "SELECT id, name, hex(data), data IS NULL FROM files_copy ORDER BY id"),
            QList<QStringList>({{"1", "a", "00FF89", "0"}, {"2", "b", "", "1"}}));
    }

    void failuresLeaveNothing()
    {
        QTemporaryDir dir;
        auto c = open(dir.filePath("target.sqlite"));
        c->query("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL)", nullptr, nullptr);
        c->query("INSERT INTO t VALUES (100, 'kept')", nullptr, nullptr);
        const Reader csv = csvReader(kFixtures + "/import_people.csv", CsvOptions());

        /* an existing table, emptied first: the third row's NULL can't go in,
         * and the table is as it was */
        Plan p;
        p.database = "main";
        p.table = "t";
        p.emptyFirst = true;
        p.columns = {{0, "id"}, {6, "name"}};
        qint64 n = -1;
        QString error;
        QVERIFY(!run(c.get(), p, csv, &n, &error));
        QVERIFY2(error.startsWith("Nothing was imported: rows 1–3:"), qPrintable(error));
        QCOMPARE(n, qint64(0));
        QCOMPARE(rows(c.get(), "SELECT id, name FROM t"), QList<QStringList>({{"100", "kept"}}));

        /* the same into a new table: it goes again */
        Plan made = planFor("made", {"id", "name"}, {});
        made.columns[1].source = 6;
        made.columns[1].type.kind = ColumnType::Text;
        /* a mapping past the source's columns: stopped before any row */
        Plan bad = made;
        bad.columns.append({9, "nowhere"});
        QVERIFY(!run(c.get(), bad, csv, &n, &error));
        QVERIFY2(error.contains("the source has no column 10"), qPrintable(error));
        QCOMPARE(rows(c.get(), "SELECT count(*) FROM sqlite_master WHERE name = 'made'"),
                 QList<QStringList>({{"0"}}));

        /* cancelled: rolled back and the new table gone too */
        made.rowsPerInsert = 1;
        QVERIFY(!run(c.get(), made, csv, &n, &error, {}, [] { return true; }));
        QVERIFY(error.startsWith("Cancelled"));
        QCOMPARE(rows(c.get(), "SELECT count(*) FROM sqlite_master WHERE name = 'made'"),
                 QList<QStringList>({{"0"}}));

        /* a table that's already there isn't made again */
        Plan again = planFor("t", {"id"}, {});
        QVERIFY(!run(c.get(), again, csv, &n, &error));
        QVERIFY(error.startsWith("Can't create the table"));
        QCOMPARE(rows(c.get(), "SELECT count(*) FROM t"), QList<QStringList>({{"1"}}));
    }
};

QTEST_GUILESS_MAIN(TestExternalData)
#include "tst_externaldata.moc"
