/* OpenYog — standalone selftests: result export formats, menu shortcuts, statementAt(), keyword
 * case, the embedded icons, the Custom Filter's WHERE, and autocomplete. Declared in Selftest.h;
 * qt/main.cpp runs one per flag. */
#include "Selftest.h"
#include "MainWindow.h"
#include "ConnectionParams.h"
#include "ResultExport.h"
#include "SqlSplit.h"
#include "SqlEditor.h"
#include <Qsci/qscilexersql.h>
#include "CustomFilterDialog.h"
#include "Icons.h"
#include "Theme.h"

#include <QApplication>
#include <QAction>
#include <QDir>
#include <QKeyEvent>
#include <QKeySequence>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QTextStream>

namespace Selftest {

/* --exporttest=DIR — write a fixed 3-row grid in every format and
 * check the output has the expected shape/markers */
int exportTest(const QString &dir)
{
    const QStringList headers = {QStringLiteral("id"), QStringLiteral("name"),
                                 QStringLiteral("note")};
    const QStringList vals = {QStringLiteral("1"), QStringLiteral("Ann,B"),  QStringLiteral("ok"),
                              QStringLiteral("2"), QStringLiteral("Q\"x\""), QStringLiteral("NULL"),
                              QStringLiteral("3"), QStringLiteral("<t>"),    QStringLiteral("a'b")};
    const auto cell = [&](int r, int c) { return vals.at(r * 3 + c); };
    struct T
    {
        ResultExport::Format f;
        const char *name, *must;
    };
    const QList<T> ts = {
        {ResultExport::Format::Csv, "csv", "\"Ann,B\""},
        {ResultExport::Format::Tsv, "tsv", "Ann,B\t"},
        {ResultExport::Format::Html, "html", "<td>&lt;t&gt;</td>"},
        {ResultExport::Format::Json, "json", "\"note\": null"},
        {ResultExport::Format::Markdown, "md", "| id | name | note |"},
        {ResultExport::Format::Xml, "xml", "<note xsi:nil=\"true\"/>"},
        {ResultExport::Format::Sql, "sql", "'a\\'b'"},
        {ResultExport::Format::Excel, "xls", "mso-application"},
    };
    bool ok = true;
    ResultExport::Options opt;
    for(const T &t : ts) {
        const QString p = dir + QStringLiteral("/exporttest.") + t.name;
        QString err;
        bool w = ResultExport::write(p, t.f, headers, cell, 3, 3, opt, &err);
        QString body;
        if(w) {
            QFile fh(p);
            if(fh.open(QIODevice::ReadOnly))
                body = QString::fromUtf8(fh.readAll());
        }
        const bool has = body.contains(QString::fromUtf8(t.must));
        ok = ok && w && has;
        QTextStream(stdout) << "exporttest " << t.name << ": wrote=" << w << " marker=" << has
                            << (w && has ? "  PASS\n" : "  FAIL\n");
    }
    /* SQL with structure */
    {
        ResultExport::Options so;
        so.sqlStructure = true;
        so.sqlTable = QStringLiteral("t1");
        so.sqlCreate = QStringLiteral("CREATE TABLE `t1` (`id` INT)");
        const QString p = dir + QStringLiteral("/exporttest_struct.sql");
        QString err;
        bool w = ResultExport::write(p, ResultExport::Format::Sql, headers, cell, 3, 3, so, &err);
        QString body;
        QFile fh(p);
        if(w && fh.open(QIODevice::ReadOnly))
            body = QString::fromUtf8(fh.readAll());
        const bool has = body.contains(QStringLiteral("DROP TABLE IF EXISTS `t1`")) &&
                         body.contains(QStringLiteral("CREATE TABLE `t1`")) &&
                         body.contains(QStringLiteral("INSERT INTO `t1`"));
        ok = ok && w && has;
        QTextStream(stdout) << "exporttest sql+structure: wrote=" << w << " marker=" << has
                            << (w && has ? "  PASS\n" : "  FAIL\n");
    }
    /* SQL, non-MySQL dialect: double-quoted identifiers, and the
     * embedded single quote must be doubled (ANSI) rather than
     * backslash-escaped (MySQL) — backslash isn't an escape
     * character in a Postgres/SQLite string literal by default */
    {
        ResultExport::Options po;
        po.driver = SqlDriverType::Postgres;
        po.sqlStructure = true;
        po.sqlTable = QStringLiteral("t1");
        po.sqlCreate = QStringLiteral("CREATE TABLE \"t1\" (\"id\" integer)");
        const QString p = dir + QStringLiteral("/exporttest_pg.sql");
        QString err;
        bool w = ResultExport::write(p, ResultExport::Format::Sql, headers, cell, 3, 3, po, &err);
        QString body;
        QFile fh(p);
        if(w && fh.open(QIODevice::ReadOnly))
            body = QString::fromUtf8(fh.readAll());
        const bool has =
            body.contains(QStringLiteral("DROP TABLE IF EXISTS \"t1\"")) &&
            body.contains(QStringLiteral("INSERT INTO \"t1\" (\"id\", \"name\", \"note\")")) &&
            body.contains(QStringLiteral("'a''b'")) && !body.contains(QStringLiteral("\\'"));
        ok = ok && w && has;
        QTextStream(stdout) << "exporttest sql pg-dialect: wrote=" << w << " marker=" << has
                            << (w && has ? "  PASS\n" : "  FAIL\n");
    }
    return ok ? 0 : 1;
}

/* --shortcuttest — build the main window and check the menu accelerators
 * that used to be display-only hints now resolve to real key sequences */
int shortcutTest(int &argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    MainWindow w;
    const QList<QAction *> acts = w.findChildren<QAction *>();
    int hinted = 0, wired = 0;
    const QStringList wantKeys = {QStringLiteral("F9"),           QStringLiteral("Ctrl+F9"),
                                  QStringLiteral("Ctrl+T"),       QStringLiteral("Ctrl+F"),
                                  QStringLiteral("Ctrl+Shift+E"), QStringLiteral("Ctrl+D"),
                                  QStringLiteral("F5"),           QStringLiteral("Ctrl+U")};
    QStringList haveKeys;
    for(QAction *ac : acts) {
        if(ac->text().contains(QLatin1Char('\t'))) {
            ++hinted;
            if(!ac->shortcut().isEmpty())
                ++wired;
        }
        if(!ac->shortcut().isEmpty())
            haveKeys << ac->shortcut().toString(QKeySequence::PortableText);
    }
    bool ok = true;
    for(const QString &k : wantKeys) {
        const bool has = haveKeys.contains(k);
        ok = ok && has;
        QTextStream(stdout) << "shortcuttest " << k << ": " << (has ? "PASS\n" : "FAIL\n");
    }
    QTextStream(stdout) << "shortcuttest: " << wired << "/" << hinted
                        << " hinted menu actions now have a real shortcut\n";
    return ok ? 0 : 1;
}

/* --stmtattest — statementAt() picks the right statement for a cursor */
int stmtAtTest()
{
    const QString sql = QStringLiteral("SELECT 1;\nSELECT 2;\nDELIMITER $$\n"
                                       "CREATE PROCEDURE p() BEGIN SELECT 3; END$$\n"
                                       "DELIMITER ;\nSELECT 4;\n");
    struct C
    {
        int pos;
        const char *want;
    };
    const C cs[] = {
        {3, "SELECT 1"},
        {14, "SELECT 2"},
        {55, "CREATE PROCEDURE p() BEGIN SELECT 3; END"},
        {int(sql.size()) - 2, "SELECT 4"},
    };
    bool ok = true;
    for(const C &c : cs) {
        const QString got = statementAt(sql, c.pos);
        const bool pass = got == QString::fromUtf8(c.want);
        ok = ok && pass;
        QTextStream(stdout) << "stmtattest pos " << c.pos << ": "
                            << (pass ? "PASS" : "FAIL got=[" + got + "]") << "\n";
    }
    return ok ? 0 : 1;
}

/* --casetest — keyword display case, upstream EditorFont::SetCase's
 * behavior: lowercase typed text RENDERS uppercase (SCI_STYLESETCASE
 * on the keyword style) while the buffer keeps the typed case */
int caseTest(int &argc, char **argv)
{
    QApplication app(argc, argv);
    const QString sql = QStringLiteral("select id, name from t1 where id = 1;");
    SqlEditor ed;
    ed.resize(560, 140);
    ed.setPlainText(sql);
    const int got = ed.SendScintilla(QsciScintillaBase::SCI_STYLEGETCASE, QsciLexerSQL::Keyword);
    const bool upper = got == QsciScintillaBase::SC_CASE_UPPER;
    const bool kept = ed.text() == sql;
    ed.grab().save(QStringLiteral("/tmp/openyog-casetest.png"));
    QTextStream(stdout) << "casetest: keyword-style-case=" << got << " ("
                        << (upper ? "UPPER" : "NOT-UPPER")
                        << "), buffer-kept=" << (kept ? "yes" : "NO")
                        << ", shot=/tmp/openyog-casetest.png\n";
    return upper && kept ? 0 : 1;
}

/* --icontest — every bitmap the UI draws actually decodes. Catches two
 * failure modes that are both silent at runtime, because a QIcon that
 * cannot load just draws nothing and leaves a text-only toolbar:
 *   1. include/bitmaps not compiled into the binary (the files used to
 *      be read from <exe>/../include/bitmaps, a path that exists only
 *      inside a build tree — never in a deployed Windows folder);
 *   2. .ico decoding unavailable. ICO is an imageformats PLUGIN, not a
 *      built-in QtGui format, so a Windows deploy without
 *      imageformats/qico.dll fails every icon even with the files
 *      embedded. Linux never sees this — the distro Qt has the plugin.
 * Decoding is forced via pixmap(): QIcon::isNull() is false for any
 * icon built from a filename, present or not, so it proves nothing. */
int iconTest(int &argc, char **argv)
{
    QApplication app(argc, argv);
    const QStringList embedded = QDir(QStringLiteral(":/bitmaps")).entryList(QDir::Files);
    QStringList failed;
    for(const QString &f : embedded)
        if(QIcon(Icons::path(f)).pixmap(16, 16).isNull())
            failed << f;
    /* a few names the main toolbar and browser tree ask for by hand,
     * so a glob that silently matched nothing can't pass this */
    const QStringList required = {
        QStringLiteral("execute_16.ico"), QStringLiteral("refresh_16.ico"),
        QStringLiteral("table.ico"),      QStringLiteral("discon.ICO"),
        QStringLiteral("connection.png"),
    };
    QStringList missing;
    for(const QString &f : required)
        if(QIcon(Icons::path(f)).pixmap(16, 16).isNull())
            missing << f;
    /* contact sheet of every embedded bitmap — wine swallows this
     * app's stdout, so under wine the PNG is the only readable proof
     * (same trick --casetest uses). Painted straight onto a QImage:
     * no widgets, so a fontless wine prefix can't tofu it. */
    const int cell = 34, cols = 16;
    QImage sheet(cols * cell, ((embedded.size() + cols - 1) / cols) * cell, QImage::Format_ARGB32);
    sheet.fill(Qt::white);
    {
        QPainter p(&sheet);
        for(int i = 0; i < embedded.size(); ++i)
            p.drawPixmap((i % cols) * cell + 1, (i / cols) * cell + 1,
                         QIcon(Icons::path(embedded[i])).pixmap(32, 32));
    }
    sheet.save(QStringLiteral("/tmp/openyog-icontest.png"));

    const bool ok = !embedded.isEmpty() && failed.isEmpty() && missing.isEmpty();
    QTextStream(stdout) << "icontest: embedded=" << embedded.size()
                        << " decoded=" << (embedded.size() - failed.size())
                        << " required-ok=" << (required.size() - missing.size()) << "/"
                        << required.size() << " sheet=/tmp/openyog-icontest.png\n";
    if(!failed.isEmpty())
        QTextStream(stdout) << "  undecodable: " << failed.mid(0, 8).join(u' ')
                            << (failed.size() > 8 ? " …" : "") << "\n";
    if(!missing.isEmpty())
        QTextStream(stdout) << "  MISSING required: " << missing.join(u' ') << "\n";
    if(embedded.isEmpty())
        QTextStream(stdout) << "  :/bitmaps is empty — the generated "
                               "bitmaps.qrc never made it into the binary\n";
    QTextStream(stdout) << "icontest: " << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}

int filterTest()
{
    /* Custom Filter dialog's WHERE-building — the part faithfully
     * ported from upstream src/SortAndFilter.cpp's SetFilterString()/
     * ProcessFilter(): NULL detection, LIKE's leading/trailing '%'
     * stripping, AND-joining, blank rows skipped */
    const auto qi = [](const QString &c) { return QStringLiteral("`%1`").arg(c); };
    const auto esc = [](const QString &v) { return QString(v).replace("'", "''"); };
    using Row = CustomFilterDialog::Row;
    struct C
    {
        QVector<Row> rows;
        const char *want;
    };
    const C cs[] = {
        {{{"city", "=", "Dhaka"}}, "`city` = 'Dhaka'"},
        {{{"city", "=", "NULL"}}, "`city` IS NULL"},
        {{{"city", "<>", "(null)"}}, "`city` IS NOT NULL"},
        {{{"name", "LIKE", "%foo%"}}, "`name` LIKE '%foo%'"},
        {{{"name", "LIKE", "foo%"}}, "`name` LIKE 'foo%'"},
        {{{"name", "LIKE", "%foo"}}, "`name` LIKE '%foo'"},
        {{{"name", "LIKE", "foo"}}, "`name` LIKE 'foo'"},
        {{{"id", ">", "10"}, {}, {"city", "=", "Dhaka"}}, "`id` > '10' AND `city` = 'Dhaka'"},
        {{}, ""},
    };
    bool ok = true;
    for(const C &c : cs) {
        const QString got = CustomFilterDialog::buildWhere(c.rows, qi, esc);
        const bool pass = got == QString::fromUtf8(c.want);
        ok = ok && pass;
        QTextStream(stdout) << "filtertest [" << c.want
                            << "]: " << (pass ? "PASS" : "FAIL got=[" + got + "]") << "\n";
    }
    return ok ? 0 : 1;
}

int compTest(int &argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    SqlEditor ed;
    ed.setCompletions(
        {QStringLiteral("employees"), QStringLiteral("emp_dept"), QStringLiteral("orders")});
    const auto countFor = [&ed](const QString &sql) {
        ed.setPlainText(sql);
        ed.moveCursorToEnd();
        ed.triggerCompletion();
        return ed.completionCountForTest();
    };
    /* 1. generic fallback (no schema split) */
    const int n = countFor(QStringLiteral("SELECT emp"));
    bool ok = n >= 2;
    QTextStream(stdout) << "comptest: 'SELECT emp' = " << n << (ok ? "  PASS\n" : "  FAIL\n");
    /* 2. clause-aware routing */
    ed.setCompletions({});
    ed.setSchema({QStringLiteral("employees"), QStringLiteral("emp_dept")},
                 {QStringLiteral("emp_id"), QStringLiteral("emp_name"), QStringLiteral("hire_dt")});
    const int t = countFor(QStringLiteral("SELECT * FROM emp"));
    const int col = countFor(QStringLiteral("SELECT emp"));
    const int colW = countFor(QStringLiteral("SELECT * FROM x WHERE emp"));
    const bool okT = t == 2, okC = col == 2 && colW == 2;
    QTextStream(stdout) << "comptest: FROM->tables 'emp' = " << t << (okT ? "  PASS" : "  FAIL")
                        << " | SELECT/WHERE->columns 'emp' = " << col << "/" << colW
                        << (okC ? "  PASS\n" : "  FAIL\n");
    /* 3. columns of the statement's own tables lead, "alias." narrows */
    ed.setSchema({QStringLiteral("employees"), QStringLiteral("orders")},
                 {QStringLiteral("emp_id"), QStringLiteral("emp_name"), QStringLiteral("o_id"),
                  QStringLiteral("o_total"), QStringLiteral("aaa_unrelated")});
    ed.setColumnLookup([](const QString &t) {
        return t == QStringLiteral("employees")
                   ? QStringList{QStringLiteral("emp_id"), QStringLiteral("emp_name")}
                   : QStringList{QStringLiteral("o_id"), QStringLiteral("o_total")};
    });
    const auto hitsFor = [&ed](const QString &sql) {
        ed.setPlainText(sql);
        ed.moveCursorToEnd();
        ed.triggerCompletion();
        return ed.completionHitsForTest();
    };
    const QStringList w1 = hitsFor(QStringLiteral("SELECT * FROM orders WHERE "));
    const bool ok1 =
        w1.mid(0, 2) == QStringList{QStringLiteral("o_id"), QStringLiteral("o_total")} &&
        w1.contains(QStringLiteral("aaa_unrelated"));
    ed.setPlainText(QStringLiteral("SELECT e. FROM employees e JOIN orders o ON 1=1"));
    ed.setCursorPosition(0, 9);
    ed.triggerCompletion();
    const QStringList w3 = ed.completionHitsForTest();
    const bool ok3 = w3 == QStringList{QStringLiteral("emp_id"), QStringLiteral("emp_name")};
    QTextStream(stdout) << "comptest: WHERE leads with own-table columns = "
                        << w1.mid(0, 3).join(',') << (ok1 ? "  PASS" : "  FAIL")
                        << " | 'e.' narrows to employees = " << w3.join(',')
                        << (ok3 ? "  PASS\n" : "  FAIL\n");
    /* 4. typing, through keyPressEvent: "." opens the qualifier's
     * columns by itself; nothing pops up inside a string literal */
    const auto typeKey = [&ed](const QString &sql, int key, const QString &text) {
        ed.setPlainText(sql);
        ed.moveCursorToEnd();
        QKeyEvent k(QEvent::KeyPress, key, Qt::NoModifier, text);
        QCoreApplication::sendEvent(&ed, &k);
        return ed.completionHitsForTest();
    };
    const QStringList dot = typeKey(QStringLiteral("SELECT * FROM orders o WHERE o"),
                                    Qt::Key_Period, QStringLiteral("."));
    const bool okDot = dot == QStringList{QStringLiteral("o_id"), QStringLiteral("o_total")};
    const bool okStr =
        typeKey(QStringLiteral("SELECT * FROM orders WHERE x = 'o_"), Qt::Key_T,
                QStringLiteral("t"))
            .isEmpty() &&
        !typeKey(QStringLiteral("SELECT * FROM orders WHERE o_"), Qt::Key_T, QStringLiteral("t"))
             .isEmpty();
    /* 5. a caret several lines down: the offset has to count the
     * newlines before it, or the context comes from the wrong spot */
    const QStringList down =
        hitsFor(QStringLiteral("SELECT 1;\n\n\n\nSELECT * FROM employees e WHERE e."));
    const bool okDown = down == QStringList{QStringLiteral("emp_id"), QStringLiteral("emp_name")};
    QTextStream(stdout) << "comptest: 'e.' four lines down = " << down.join(',')
                        << (okDown ? "  PASS\n" : "  FAIL\n");
    QTextStream(stdout) << "comptest: typing 'o.' pops orders columns = " << dot.join(',')
                        << (okDot ? "  PASS" : "  FAIL") << " | no popup inside a string"
                        << (okStr ? "  PASS\n" : "  FAIL\n");
    return (ok && okT && okC && ok1 && ok3 && okDot && okStr && okDown) ? 0 : 1;
}

} // namespace Selftest
