/* OpenYog — unit tests for qt/DelimitedText (the CSV/TSV parser behind
 * Import ▸ CSV on SQLite/PostgreSQL), including round trips through qt/ResultExport's
 * writer: what Export writes, Import must read back unchanged. */
#include "DelimitedText.h"
#include "ResultExport.h"

#include <QTest>

using Rows = QList<QStringList>;

class TestDelimitedText : public QObject
{
    Q_OBJECT

private slots:
    void parse_data();
    void parse();
    void roundTripQuoted();
    void roundTripEscapedBy();
};

void TestDelimitedText::parse_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QChar>("sep");
    QTest::addColumn<QChar>("quote");
    QTest::addColumn<QChar>("esc");
    QTest::addColumn<Rows>("expected");

    const QChar comma(QLatin1Char(',')), dq(QLatin1Char('"')), bs(QLatin1Char('\\')), none;

    QTest::newRow("simple") << "a,b\nc,d\n" << comma << dq << none << Rows{{"a", "b"}, {"c", "d"}};
    QTest::newRow("no trailing newline")
        << "a,b\nc,d" << comma << dq << none << Rows{{"a", "b"}, {"c", "d"}};
    QTest::newRow("CRLF") << "a,b\r\nc,d\r\n"
                          << comma << dq << none << Rows{{"a", "b"}, {"c", "d"}};
    QTest::newRow("empty fields") << ",,\n" << comma << dq << none << Rows{{"", "", ""}};
    QTest::newRow("quoted separator") << "\"a,b\",c\n" << comma << dq << none << Rows{{"a,b", "c"}};
    QTest::newRow("doubled quote") << "\"say \"\"hi\"\"\",x\n"
                                   << comma << dq << none << Rows{{"say \"hi\"", "x"}};
    QTest::newRow("newline inside quotes") << "\"l1\r\nl2\",x\n"
                                           << comma << dq << none << Rows{{"l1\r\nl2", "x"}};
    QTest::newRow("no quote char") << "\"a\",b\n" << comma << none << none << Rows{{"\"a\"", "b"}};
    QTest::newRow("tab separated") << "a\tb,c\n"
                                   << QChar(QLatin1Char('\t')) << dq << none << Rows{{"a", "b,c"}};
    QTest::newRow("escape inside quotes") << "\"a\\\"b\",c\n"
                                          << comma << dq << bs << Rows{{"a\"b", "c"}};
    QTest::newRow("escaped separator, unquoted") << "a\\,b,c\n"
                                                 << comma << dq << bs << Rows{{"a,b", "c"}};
    QTest::newRow("escaped quote, unquoted") << "say \\\"hi\\\",x\n"
                                             << comma << dq << bs << Rows{{"say \"hi\"", "x"}};
    QTest::newRow("control sequences") << "l1\\nl2\\r\\tz,\"q\\n\"\n"
                                       << comma << dq << bs << Rows{{"l1\nl2\r\tz", "q\n"}};
    QTest::newRow("escaped escape") << "c:\\\\dir,x\n"
                                    << comma << dq << bs << Rows{{"c:\\dir", "x"}};
    QTest::newRow("\\N kept as written") << "\\N,x\n" << comma << dq << bs << Rows{{"\\N", "x"}};
    QTest::newRow("escape off: backslash is data") << "a\\,b\n"
                                                   << comma << dq << none << Rows{{"a\\", "b"}};
}

void TestDelimitedText::parse()
{
    QFETCH(QString, text);
    QFETCH(QChar, sep);
    QFETCH(QChar, quote);
    QFETCH(QChar, esc);
    QFETCH(Rows, expected);
    QCOMPARE(parseDelimitedText(text, sep, quote, esc), expected);
}

namespace {
/* the values most likely to break a round trip */
const QStringList kHeaders{"id", "text"};
const Rows kRows{{"1", "plain"},      {"2", "a,b"},         {"3", "say \"hi\""},
                 {"4", "two\nlines"}, {"5", "crlf\r\nend"}, {"6", "back\\slash"},
                 {"7", ""},           {"8", "বাংলা, ✓"}};

QString exportCsv(const ResultExport::Options &opt)
{
    return ResultExport::render(
        ResultExport::Format::Csv, kHeaders, [](int r, int c) { return kRows[r][c]; },
        int(kRows.size()), int(kHeaders.size()), opt);
}
} // namespace

void TestDelimitedText::roundTripQuoted()
{
    /* the export dialog's defaults: quote-doubling, CRLF line ends */
    const Rows back =
        parseDelimitedText(exportCsv({}), QLatin1Char(','), QLatin1Char('"'), QChar());
    QCOMPARE(back, Rows{kHeaders} + kRows);
}

void TestDelimitedText::roundTripEscapedBy()
{
    /* "Escaped by \": nothing is quoted, separators/quotes/escapes are
     * backslash-prefixed and newlines written as \n / \r — the MySQL
     * FIELDS ESCAPED BY convention ResultExport.h promises round-trips */
    ResultExport::Options opt;
    opt.escapeChar = QLatin1Char('\\');
    opt.lineEnd = QStringLiteral("\n");
    const Rows back =
        parseDelimitedText(exportCsv(opt), QLatin1Char(','), QLatin1Char('"'), QLatin1Char('\\'));
    QCOMPARE(back, Rows{kHeaders} + kRows);
}

QTEST_GUILESS_MAIN(TestDelimitedText)
#include "tst_delimitedtext.moc"
