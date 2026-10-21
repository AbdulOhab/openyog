/* OpenYog — unit tests for qt/ResultExport (the CSV/TSV/HTML/JSON/Markdown/
 * XML/SQL/Excel writers behind "Export result as…" and Table Data's
 * Export…). */
#include "ResultExport.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

using ResultExport::Format;
using ResultExport::Options;

namespace {
/* a fixed grid: render(…, cell, rows, cols, …) reads it through a lambda,
 * the same way the result grid and Table Data pane feed it */
struct Grid
{
    QStringList headers;
    QList<QStringList> rows;

    QString render(Format fmt, const Options &opt = {}) const
    {
        return ResultExport::render(
            fmt, headers, [this](int r, int c) { return rows[r][c]; }, int(rows.size()),
            int(headers.size()), opt);
    }
};
} // namespace

class TestResultExport : public QObject
{
    Q_OBJECT

private slots:
    void csvQuoting();
    void csvEscapedByMode();
    void csvNoHeaderAndCustomDelimiter();
    void csvBom();
    void tsvUsesTab();
    void htmlEscapes();
    void jsonNullsAndValues();
    void xmlNilAndEscaping();
    void markdownEscapesPipes();
    void sqlMysqlLiterals();
    void sqlAnsiLiterals();
    void sqlStructureAndBatching();
    void writeMatchesRender();
    void writeFailureReportsError();
    void formatNamesRoundTrip();
};

void TestResultExport::csvQuoting()
{
    const Grid g{{"id", "note"},
                 {{"1", "plain"}, {"2", "a,b"}, {"3", "say \"hi\""}, {"4", "two\nlines"}}};
    QCOMPARE(g.render(Format::Csv), QStringLiteral("id,note\r\n"
                                                   "1,plain\r\n"
                                                   "2,\"a,b\"\r\n"
                                                   "3,\"say \"\"hi\"\"\"\r\n"
                                                   "4,\"two\nlines\"\r\n"));
}

void TestResultExport::csvEscapedByMode()
{
    Options opt;
    opt.escapeChar = QLatin1Char('\\');
    opt.lineEnd = QStringLiteral("\n");
    const Grid g{{"v"}, {{"a,b"}, {"q\"q"}, {"back\\slash"}, {"l1\nl2\r"}}};
    QCOMPARE(g.render(Format::Csv, opt), QStringLiteral("v\n"
                                                        "a\\,b\n"
                                                        "q\\\"q\n"
                                                        "back\\\\slash\n"
                                                        "l1\\nl2\\r\n"));
}

void TestResultExport::csvNoHeaderAndCustomDelimiter()
{
    Options opt;
    opt.header = false;
    opt.delimiter = QLatin1Char(';');
    opt.lineEnd = QStringLiteral("\n");
    const Grid g{{"a", "b"}, {{"1", "x;y"}, {"2", "z"}}};
    QCOMPARE(g.render(Format::Csv, opt), QStringLiteral("1;\"x;y\"\n2;z\n"));
}

void TestResultExport::csvBom()
{
    Options opt;
    opt.bom = true;
    const Grid g{{"a"}, {{"1"}}};
    QVERIFY(g.render(Format::Csv, opt).startsWith(QChar(0xFEFF)));
    QVERIFY(!g.render(Format::Csv).startsWith(QChar(0xFEFF)));
}

void TestResultExport::tsvUsesTab()
{
    Options opt;
    opt.lineEnd = QStringLiteral("\n");
    const Grid g{{"a", "b"}, {{"1", "x,y"}}};
    /* the comma is not special in TSV, so no quoting */
    QCOMPARE(g.render(Format::Tsv, opt), QStringLiteral("a\tb\n1\tx,y\n"));
}

void TestResultExport::htmlEscapes()
{
    const Grid g{{"<h>"}, {{"a & <b>"}}};
    const QString html = g.render(Format::Html);
    QVERIFY(html.contains(QStringLiteral("<th>&lt;h&gt;</th>")));
    QVERIFY(html.contains(QStringLiteral("<td>a &amp; &lt;b&gt;</td>")));
}

void TestResultExport::jsonNullsAndValues()
{
    const Grid g{{"id", "name"}, {{"1", "Alice"}, {"2", "NULL"}}};
    const QJsonDocument doc = QJsonDocument::fromJson(g.render(Format::Json).toUtf8());
    QVERIFY(doc.isArray());
    const QJsonArray arr = doc.array();
    QCOMPARE(arr.size(), 2);
    QCOMPARE(arr[0].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Alice"));
    /* values stay strings — the grid has no column types to go on */
    QCOMPARE(arr[0].toObject().value(QStringLiteral("id")).toString(), QStringLiteral("1"));
    QVERIFY(arr[1].toObject().value(QStringLiteral("name")).isNull());
}

void TestResultExport::xmlNilAndEscaping()
{
    const Grid g{{"first name", "v"}, {{"a<b", "NULL"}}};
    const QString xml = g.render(Format::Xml);
    QVERIFY(xml.contains(QStringLiteral("<first_name>a&lt;b</first_name>")));
    QVERIFY(xml.contains(QStringLiteral("<v xsi:nil=\"true\"/>")));
}

void TestResultExport::markdownEscapesPipes()
{
    const Grid g{{"a|b"}, {{"x|y"}}};
    QCOMPARE(g.render(Format::Markdown), QStringLiteral("| a\\|b |\n| --- |\n| x\\|y |\n"));
}

void TestResultExport::sqlMysqlLiterals()
{
    Options opt;
    opt.sqlTable = QStringLiteral("t");
    const Grid g{{"id", "s"}, {{"1", "it's"}, {"2", "a\\b\nc"}, {"3", "NULL"}}};
    QCOMPARE(g.render(Format::Sql, opt), QStringLiteral("INSERT INTO `t` (`id`, `s`) VALUES\n"
                                                        "  ('1', 'it\\'s'),\n"
                                                        "  ('2', 'a\\\\b\\nc'),\n"
                                                        "  ('3', NULL);\n"));
}

void TestResultExport::sqlAnsiLiterals()
{
    /* Postgres/SQLite: quote doubled, backslash and newline left alone */
    Options opt;
    opt.sqlTable = QStringLiteral("t");
    opt.driver = SqlDriverType::Postgres;
    const Grid g{{"s"}, {{"it's a\\b\nc"}}};
    QCOMPARE(g.render(Format::Sql, opt),
             QStringLiteral("INSERT INTO \"t\" (\"s\") VALUES\n  ('it''s a\\b\nc');\n"));
}

void TestResultExport::sqlStructureAndBatching()
{
    Options opt;
    opt.sqlTable = QStringLiteral("t");
    opt.sqlStructure = true;
    opt.sqlCreate = QStringLiteral("CREATE TABLE `t` (`n` int)");
    Grid g{{"n"}, {}};
    for(int i = 0; i < 201; ++i)
        g.rows << QStringList{QString::number(i)};
    const QString sql = g.render(Format::Sql, opt);
    QVERIFY(
        sql.startsWith(QStringLiteral("DROP TABLE IF EXISTS `t`;\nCREATE TABLE `t` (`n` int);\n")));
    /* 200 rows per INSERT: 201 rows → two statements */
    QCOMPARE(sql.count(QStringLiteral("INSERT INTO")), 2);
    QVERIFY(sql.endsWith(QStringLiteral("  ('200');\n")));
}

void TestResultExport::writeMatchesRender()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("out.csv"));
    Options opt;
    opt.bom = true;
    const Grid g{{"name"}, {{"Đặng"}, {"বাংলা"}}};

    QString err;
    QVERIFY2(ResultExport::write(
                 path, Format::Csv, g.headers, [&g](int r, int c) { return g.rows[r][c]; },
                 int(g.rows.size()), 1, opt, &err),
             qPrintable(err));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    /* UTF-8 on disk, BOM included, byte-for-byte what render() produced
     * (compared as bytes: QString::fromUtf8 would silently eat the BOM) */
    QVERIFY(bytes.startsWith("\xEF\xBB\xBF"));
    QCOMPARE(bytes, g.render(Format::Csv, opt).toUtf8());
}

void TestResultExport::writeFailureReportsError()
{
    QString err;
    const Grid g{{"a"}, {{"1"}}};
    QVERIFY(!ResultExport::write(
        QStringLiteral("/nonexistent-dir/x.csv"), Format::Csv, g.headers,
        [&g](int r, int c) { return g.rows[r][c]; }, 1, 1, {}, &err));
    QVERIFY(!err.isEmpty());
}

void TestResultExport::formatNamesRoundTrip()
{
    const QStringList names = ResultExport::formatNames();
    QCOMPARE(names.size(), 8);
    QSet<QString> suffixes;
    for(const QString &n : names)
        suffixes << ResultExport::suffixFor(ResultExport::formatForName(n));
    /* every combo entry maps to its own format */
    QCOMPARE(suffixes.size(), names.size());
    QCOMPARE(ResultExport::formatForName(QStringLiteral("something else")), Format::Csv);
}

QTEST_GUILESS_MAIN(TestResultExport)
#include "tst_resultexport.moc"
