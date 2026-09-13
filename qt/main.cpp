/* OpenYog — application entry point.
 *
 * Selftest mode for CI/headless verification:
 *   openyog --screenshot=FILE.png              render main window to FILE, exit
 *   openyog --screenshot=FILE.png --dialog     render the connection dialog
 *   openyog --screenshot=FILE.png --dialog --dialogdriver=sqlite   … with SQLite preselected
 *   openyog --screenshot=FILE.png --createtable render the Create Table dialog
 *   openyog --autoconnect=h:p:u:pw:db [--opentable=db:t] [--editcell=r:c:v |
 *           --stagecell=r:c:v] --screenshot=FILE.png   drive the data grid
 *   openyog --autoconnect=… --dumpdb=FILE.sql          dump a database
 *   openyog --delconn=NAME                             delete a saved connection
 *   openyog --screenshot=FILE.png --indexdlg           render the Manage Indexes dialog
 *   openyog --fmtsql="SELECT …"                        print the formatted SQL, exit
 *   openyog --comptest                                 autocomplete self-check
 *   openyog --sqlitetest=FILE.sqlite                   SQLite driver shape self-check
 *   openyog --autoconnectfile=FILE.sqlite [--opentable=db:t] [--editcell=r:c:v]
 *           --screenshot=FILE.png       same data-grid selftests, SQLite driver
 */
#include "MainWindow.h"
#include "ConnectionDialog.h"
#include "ConnectionParams.h"
#include "ConnectionStore.h"
#include "CreateTableDialog.h"
#include "IndexDialog.h"
#include "ExportDialog.h"
#include "ResultExport.h"
#include "SchemaSql.h"
#include "SqlSplit.h"
#include "SqlFormat.h"
#include "CodeEditor.h"
#include "Theme.h"
#include "db/IDbDriver.h"
#include "db/IDbConnection.h"

#include <QDebug>

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QKeySequence>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QTextStream>
#include <QTimer>

#include <memory>

int main(int argc, char *argv[])
{
    QString screenshot;
    bool shotDialog = false;
    QString dialogDriver;   /* --dialogdriver=sqlite : preselect a driver, --dialog selftest */
    bool shotCreateTable = false;
    bool shotIndexDlg = false;
    bool shotExportDlg = false;
    QPair<QString, QString> openTableParts;
    QString dataViewMode;   /* --dataview=text|grid selftest */
    QString checkRows;      /* --checkrows=0,2,4 selftest */
    QString hexCell;        /* --hexcell=row:col:hexdigits selftest */
    QString mkObj;          /* --mkobj=VIEW|PROCEDURE|… selftest */
    int editRow = -1, editCol = -1;
    bool stageOnly = false;
    QString editValue;
    ConnectionParams autoConnect;
    bool doAutoConnect = false;
    QString dumpPath;
    QString copyDbArg;
    QString sqliteCopyDbArg;
    QString sqliteCsvImportArg;
    QString schemaHtmlArg;
    QString delConn;
    for(int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if(a.startsWith(QStringLiteral("--screenshot=")))
            screenshot = a.mid(QStringLiteral("--screenshot=").size());
        if(a == QStringLiteral("--dialog"))
            shotDialog = true;
        if(a.startsWith(QStringLiteral("--dialogdriver=")))
            dialogDriver = a.mid(QStringLiteral("--dialogdriver=").size());
        if(a == QStringLiteral("--createtable"))
            shotCreateTable = true;
        if(a == QStringLiteral("--indexdlg"))
            shotIndexDlg = true;
        if(a == QStringLiteral("--exportdlg"))
            shotExportDlg = true;
        /* --opentable=db:table (selftest: opens the editable data grid) */
        /* --editcell=row:col:value  stage the edit AND apply it (UPDATE path) */
        /* --stagecell=row:col:value stage only (shows the amber cell + Apply bar) */
        if(a.startsWith(QStringLiteral("--editcell="))
           || a.startsWith(QStringLiteral("--stagecell="))) {
            stageOnly = a.startsWith(QStringLiteral("--stagecell="));
            const QStringList parts = a.section('=', 1).split(':');
            if(parts.size() == 3) {
                editRow = parts[0].toInt();
                editCol = parts[1].toInt();
                editValue = parts[2];
            }
        }
        if(a.startsWith(QStringLiteral("--dumpdb=")))
            dumpPath = a.mid(QStringLiteral("--dumpdb=").size());
        if(a.startsWith(QStringLiteral("--copydb=")))
            copyDbArg = a.mid(QStringLiteral("--copydb=").size());
        if(a.startsWith(QStringLiteral("--sqlitecopydb=")))
            sqliteCopyDbArg = a.mid(QStringLiteral("--sqlitecopydb=").size());
        if(a.startsWith(QStringLiteral("--sqlitecsvimport=")))
            sqliteCsvImportArg = a.mid(QStringLiteral("--sqlitecsvimport=").size());
        if(a.startsWith(QStringLiteral("--schemahtmltest=")))
            schemaHtmlArg = a.mid(QStringLiteral("--schemahtmltest=").size());
        if(a.startsWith(QStringLiteral("--delconn=")))
            delConn = a.mid(QStringLiteral("--delconn=").size());
        if(a.startsWith(QStringLiteral("--fmtsql="))) {
            QTextStream(stdout) << SqlFormat::pretty(a.mid(9)) << '\n';
            return 0;
        }
        /* --sqlitetest=FILE — exercise the SQLite driver's canonical metadata
         * shapes (qt/db/IDbConnection.h) headlessly against a prepared SQLite
         * file; expects the xnote/sample.sqlite layout (employees: INTEGER
         * PRIMARY KEY id, a city index, an inline FK to emp_dept, a BEFORE
         * INSERT trigger; plus a view; no routines/events). */
        if(a.startsWith(QStringLiteral("--sqlitetest="))) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QApplication app2(argc, argv);
            ConnectionParams cp;
            cp.driverType = DriverType::Sqlite;
            cp.filePath = a.mid(QStringLiteral("--sqlitetest=").size());
            QString connectError;
            IDbConnection *c = dbDriverFor(cp.driverType)->connect(cp, &connectError);
            if(!c) {
                QTextStream(stdout) << "sqlitetest: connect failed: "
                                    << connectError << '\n';
                return 1;
            }
            int fails = 0;
            const auto check = [&](bool ok, const QString &what) {
                if(!ok) ++fails;
                QTextStream(stdout) << "sqlitetest " << what
                                    << (ok ? "  PASS\n" : "  FAIL\n");
            };
            const auto findRow = [](const DbResultSet &rs, int col,
                                    const QString &val) {
                for(const QStringList &row : rs.rows)
                    if(row.value(col) == val)
                        return row;
                return QStringList{};
            };

            check(c->listDatabases().contains(QStringLiteral("main")),
                  "databases contain main");

            const QStringList tbls = c->listTables(QStringLiteral("main"));
            check(tbls.contains(QStringLiteral("employees"))
                  && tbls.contains(QStringLiteral("emp_dept"))
                  && !tbls.contains(QStringLiteral("v_employee_dept")),
                  "listTables base tables");
            const QStringList views = c->listTables(QStringLiteral("main"),
                                                    QStringLiteral("VIEW"));
            check(views.contains(QStringLiteral("v_employee_dept"))
                  && views.size() == 1, "listTables views");

            const DbResultSet cols = c->listColumns(QStringLiteral("main"),
                                                    QStringLiteral("employees"));
            const QStringList idCol = findRow(cols, 0, QStringLiteral("id"));
            check(idCol.value(1) == QStringLiteral("INTEGER")
                  && idCol.value(2) == QStringLiteral("NO")
                  && idCol.value(3) == QStringLiteral("PRI")
                  && idCol.value(5).contains(QStringLiteral("auto_increment")),
                  "listColumns id shape");
            check(findRow(cols, 0, QStringLiteral("name")).value(2)
                      == QStringLiteral("NO")
                  && findRow(cols, 0, QStringLiteral("salary")).value(2)
                         == QStringLiteral("YES"),
                  "listColumns nullability");

            const DbResultSet ixs = c->listIndexes(QStringLiteral("main"),
                                                   QStringLiteral("employees"));
            const QStringList pk = findRow(ixs, 2, QStringLiteral("PRIMARY"));
            check(pk.value(1) == QStringLiteral("0")
                  && pk.value(4) == QStringLiteral("id"),
                  "listIndexes PRIMARY (rowid alias)");
            const QStringList cityIx = findRow(ixs, 2,
                                               QStringLiteral("idx_employees_city"));
            check(cityIx.value(1) == QStringLiteral("1")
                  && cityIx.value(4) == QStringLiteral("city"),
                  "listIndexes secondary index");

            const DbResultSet fks = c->listForeignKeys(QStringLiteral("main"),
                                                       QStringLiteral("employees"));
            check(!fks.rows.isEmpty()
                  && fks.rows.first().value(1) == QStringLiteral("dept_id")
                  && fks.rows.first().value(2) == QStringLiteral("emp_dept")
                  && fks.rows.first().value(3) == QStringLiteral("id"),
                  "listForeignKeys shape");

            const DbResultSet trgs = c->listTableTriggers(
                QStringLiteral("main"), QStringLiteral("employees"));
            check(trgs.rows.size() == 1
                  && trgs.rows.first().value(0)
                         == QStringLiteral("trg_employees_no_negative_salary")
                  && trgs.rows.first().value(1) == QStringLiteral("BEFORE")
                  && trgs.rows.first().value(2) == QStringLiteral("INSERT"),
                  "listTableTriggers shape");
            check(c->listTriggers(QStringLiteral("main"))
                      .contains(QStringLiteral("trg_employees_no_negative_salary")),
                  "listTriggers");

            check(c->listRoutines(QStringLiteral("main")).rows.isEmpty(),
                  "listRoutines empty on SQLite");
            check(c->listEvents(QStringLiteral("main")).isEmpty(),
                  "listEvents empty on SQLite");

            QString ddlErr;
            check(c->showCreate(QStringLiteral("TABLE"), QStringLiteral("main"),
                                QStringLiteral("employees"), &ddlErr)
                      .startsWith(QStringLiteral("CREATE TABLE")),
                  "showCreate TABLE");
            check(!c->showCreate(QStringLiteral("VIEW"), QStringLiteral("main"),
                                 QStringLiteral("v_employee_dept"), &ddlErr).isEmpty(),
                  "showCreate VIEW");
            const QString procDdl = c->showCreate(
                QStringLiteral("PROCEDURE"), QStringLiteral("main"),
                QStringLiteral("nope"), &ddlErr);
            check(procDdl.isEmpty() && !ddlErr.isEmpty(),
                  "showCreate PROCEDURE rejected");

            check(c->sqlInsertDefaults(QStringLiteral("main"),
                                       QStringLiteral("t"))
                      .contains(QStringLiteral("DEFAULT VALUES")),
                  "sqlInsertDefaults shape");
            check(c->sqlFkChecks(false).startsWith(QStringLiteral("PRAGMA")),
                  "sqlFkChecks shape");
            check(!c->supportsLimitOnUpdateDelete(),
                  "supportsLimitOnUpdateDelete false");

            /* DML round-trip on a throwaway copy, using exactly the statement
             * shapes the table-data pane builds (quoteIdent'd, no LIMIT on
             * UPDATE/DELETE, DEFAULT VALUES insert) */
            {
                const QString tmp = cp.filePath + QStringLiteral(".dmltmp");
                QFile::remove(tmp);
                check(QFile::copy(cp.filePath, tmp), "dml: temp copy");
                ConnectionParams cp2 = cp;
                cp2.filePath = tmp;
                IDbConnection *c2 = dbDriverFor(cp2.driverType)->connect(cp2, &connectError);
                check(c2 != nullptr, "dml: reopen copy");
                if(c2) {
                    QString e;
                    bool ok = c2->query(QStringLiteral(
                        "CREATE TABLE \"main\".\"dml_t\" "
                        "(id INTEGER PRIMARY KEY, name TEXT)"), nullptr, &e);
                    ok = c2->query(c2->sqlInsertDefaults(QStringLiteral("main"),
                                                         QStringLiteral("dml_t")),
                                   nullptr, &e) && ok;
                    ok = c2->query(QStringLiteral(
                        "UPDATE \"main\".\"dml_t\" SET \"name\" = 'x' "
                        "WHERE \"id\" = 1"), nullptr, &e) && ok;
                    ok = c2->query(QStringLiteral(
                        "DELETE FROM \"main\".\"dml_t\" WHERE \"name\" = 'x'"),
                        nullptr, &e) && ok;
                    DbResultSet rs;
                    ok = c2->query(QStringLiteral("SELECT COUNT(*) FROM \"main\".\"dml_t\""),
                                   &rs, &e) && ok
                         && rs.rows.first().value(0) == QStringLiteral("0");
                    check(ok, "dml: defaults/UPDATE/DELETE round-trip");
                    if(!ok)
                        QTextStream(stdout) << "  last error: " << e << '\n';
                    delete c2;
                }
                QFile::remove(tmp);
            }

            delete c;
            return fails == 0 ? 0 : 1;
        }
        /* --schematest=host:port:user:pw:db — exercise the schema-object DDL
         * helpers (createTemplate / stripDefiner / alterStatements) against a
         * live server: create → alter → drop a View / Procedure / Function /
         * Trigger / Event, checking information_schema at each step. */
        if(a.startsWith(QStringLiteral("--schematest="))) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QApplication app2(argc, argv);
            const QStringList p = a.mid(13).split(':');
            if(p.size() != 5) {
                QTextStream(stdout) << "schematest: need host:port:user:pw:db\n";
                return 2;
            }
            ConnectionParams cp;
            cp.host = p[0]; cp.port = p[1].toUInt();
            cp.user = p[2]; cp.password = p[3]; cp.database = p[4];
            QString connectError;
            IDbConnection *c = dbDriverFor(cp.driverType)->connect(cp, &connectError);
            if(!c) {
                QTextStream(stdout) << "schematest: connect failed: "
                                    << connectError << '\n';
                return 1;
            }
            const QString db = p[4];
            auto run = [&](const QString &sql) {
                return c->query(sql, nullptr, nullptr);
            };
            auto exists = [&](const QString &q) {
                DbResultSet rs;
                if(!c->query(q, &rs, nullptr)) return -1;
                return rs.rows.isEmpty() ? 0 : rs.rows.first().value(0).toInt();
            };
            struct Case { QString kw, name, iq; };
            const QList<Case> cases = {
                { QStringLiteral("VIEW"), QStringLiteral("oy_st_view"),
                  QStringLiteral("SELECT COUNT(*) FROM information_schema.VIEWS "
                    "WHERE TABLE_SCHEMA='%1' AND TABLE_NAME='oy_st_view'").arg(db) },
                { QStringLiteral("PROCEDURE"), QStringLiteral("oy_st_proc"),
                  QStringLiteral("SELECT COUNT(*) FROM information_schema.ROUTINES "
                    "WHERE ROUTINE_SCHEMA='%1' AND ROUTINE_NAME='oy_st_proc' "
                    "AND ROUTINE_TYPE='PROCEDURE'").arg(db) },
                { QStringLiteral("FUNCTION"), QStringLiteral("oy_st_func"),
                  QStringLiteral("SELECT COUNT(*) FROM information_schema.ROUTINES "
                    "WHERE ROUTINE_SCHEMA='%1' AND ROUTINE_NAME='oy_st_func' "
                    "AND ROUTINE_TYPE='FUNCTION'").arg(db) },
                { QStringLiteral("TRIGGER"), QStringLiteral("oy_st_trg"),
                  QStringLiteral("SELECT COUNT(*) FROM information_schema.TRIGGERS "
                    "WHERE TRIGGER_SCHEMA='%1' AND TRIGGER_NAME='oy_st_trg'").arg(db) },
                { QStringLiteral("EVENT"), QStringLiteral("oy_st_event"),
                  QStringLiteral("SELECT COUNT(*) FROM information_schema.EVENTS "
                    "WHERE EVENT_SCHEMA='%1' AND EVENT_NAME='oy_st_event'").arg(db) },
            };
            bool allOk = true;
            for(const Case &cs : cases) {
                run(QStringLiteral("DROP %1 IF EXISTS `%2`.`%3`")
                        .arg(cs.kw, db, cs.name));
                QString tmpl = SchemaSql::createTemplate(cs.kw, db);
                tmpl.replace(QStringLiteral("new_view"),    cs.name);
                tmpl.replace(QStringLiteral("new_proc"),    cs.name);
                tmpl.replace(QStringLiteral("new_func"),    cs.name);
                tmpl.replace(QStringLiteral("new_trigger"), cs.name);
                tmpl.replace(QStringLiteral("new_event"),   cs.name);
                tmpl.replace(QStringLiteral("some_table"),  QStringLiteral("employees"));
                bool cOk = true;
                for(const QString &s : splitStatements(SchemaSql::editorText(
                        cs.kw, db, cs.name, tmpl, true)))
                    cOk = run(s) && cOk;
                const bool present = exists(cs.iq) == 1;
                /* alter round-trip */
                const QString ddl = c->showCreate(cs.kw, db, cs.name, nullptr);
                bool aOk = !ddl.isEmpty();
                for(const QString &s : splitStatements(SchemaSql::editorText(
                        cs.kw, db, cs.name, SchemaSql::stripDefiner(ddl), false)))
                    aOk = run(s) && aOk;
                const bool stillThere = exists(cs.iq) == 1;
                const bool dOk = run(QStringLiteral("DROP %1 IF EXISTS `%2`.`%3`")
                                         .arg(cs.kw, db, cs.name));
                const bool gone = exists(cs.iq) == 0;
                const bool caseOk = cOk && present && aOk && stillThere && dOk && gone;
                allOk = allOk && caseOk;
                QTextStream(stdout)
                    << "schematest " << cs.kw << ": create=" << cOk
                    << " present=" << present << " alter=" << aOk
                    << " kept=" << stillThere << " drop=" << dOk << " gone=" << gone
                    << (caseOk ? "  PASS\n" : "  FAIL\n");
            }
            delete c;
            return allOk ? 0 : 1;
        }
        /* --exporttest=DIR — write a fixed 3-row grid in every format and
         * check the output has the expected shape/markers */
        if(a.startsWith(QStringLiteral("--exporttest="))) {
            const QString dir = a.mid(13);
            const QStringList headers = { QStringLiteral("id"),
                                          QStringLiteral("name"),
                                          QStringLiteral("note") };
            const QStringList vals = {
                QStringLiteral("1"), QStringLiteral("Ann,B"), QStringLiteral("ok"),
                QStringLiteral("2"), QStringLiteral("Q\"x\""), QStringLiteral("NULL"),
                QStringLiteral("3"), QStringLiteral("<t>"), QStringLiteral("a'b") };
            const auto cell = [&](int r, int c) { return vals.at(r * 3 + c); };
            struct T { ResultExport::Format f; const char *name, *must; };
            const QList<T> ts = {
                { ResultExport::Format::Csv, "csv", "\"Ann,B\"" },
                { ResultExport::Format::Tsv, "tsv", "Ann,B\t" },
                { ResultExport::Format::Html, "html", "<td>&lt;t&gt;</td>" },
                { ResultExport::Format::Json, "json", "\"note\": null" },
                { ResultExport::Format::Markdown, "md", "| id | name | note |" },
                { ResultExport::Format::Xml, "xml", "<note xsi:nil=\"true\"/>" },
                { ResultExport::Format::Sql, "sql", "'a\\'b'" },
                { ResultExport::Format::Excel, "xls", "mso-application" },
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
                QTextStream(stdout)
                    << "exporttest " << t.name << ": wrote=" << w
                    << " marker=" << has << (w && has ? "  PASS\n" : "  FAIL\n");
            }
            /* SQL with structure */
            {
                ResultExport::Options so;
                so.sqlStructure = true;
                so.sqlTable = QStringLiteral("t1");
                so.sqlCreate = QStringLiteral("CREATE TABLE `t1` (`id` INT)");
                const QString p = dir + QStringLiteral("/exporttest_struct.sql");
                QString err;
                bool w = ResultExport::write(p, ResultExport::Format::Sql, headers,
                                             cell, 3, 3, so, &err);
                QString body;
                QFile fh(p);
                if(w && fh.open(QIODevice::ReadOnly))
                    body = QString::fromUtf8(fh.readAll());
                const bool has = body.contains(QStringLiteral("DROP TABLE IF EXISTS `t1`"))
                              && body.contains(QStringLiteral("CREATE TABLE `t1`"))
                              && body.contains(QStringLiteral("INSERT INTO `t1`"));
                ok = ok && w && has;
                QTextStream(stdout) << "exporttest sql+structure: wrote=" << w
                                    << " marker=" << has
                                    << (w && has ? "  PASS\n" : "  FAIL\n");
            }
            return ok ? 0 : 1;
        }
        /* --shortcuttest — build the main window and check the menu accelerators
         * that used to be display-only hints now resolve to real key sequences */
        if(a == QStringLiteral("--shortcuttest")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QApplication app2(argc, argv);
            MainWindow w;
            const QList<QAction *> acts = w.findChildren<QAction *>();
            int hinted = 0, wired = 0;
            const QStringList wantKeys = {
                QStringLiteral("F9"), QStringLiteral("Ctrl+F9"),
                QStringLiteral("Ctrl+T"), QStringLiteral("Ctrl+F"),
                QStringLiteral("Ctrl+Shift+E"), QStringLiteral("Ctrl+D"),
                QStringLiteral("F5"), QStringLiteral("Ctrl+U") };
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
                QTextStream(stdout) << "shortcuttest " << k << ": "
                                    << (has ? "PASS\n" : "FAIL\n");
            }
            QTextStream(stdout) << "shortcuttest: " << wired << "/" << hinted
                                << " hinted menu actions now have a real shortcut\n";
            return ok ? 0 : 1;
        }
        /* --stmtattest — statementAt() picks the right statement for a cursor */
        if(a == QStringLiteral("--stmtattest")) {
            const QString sql =
                QStringLiteral("SELECT 1;\nSELECT 2;\nDELIMITER $$\n"
                               "CREATE PROCEDURE p() BEGIN SELECT 3; END$$\n"
                               "DELIMITER ;\nSELECT 4;\n");
            struct C { int pos; const char *want; };
            const C cs[] = {
                { 3,  "SELECT 1" },
                { 14, "SELECT 2" },
                { 55, "CREATE PROCEDURE p() BEGIN SELECT 3; END" },
                { int(sql.size()) - 2, "SELECT 4" },
            };
            bool ok = true;
            for(const C &c : cs) {
                const QString got = statementAt(sql, c.pos);
                const bool pass = got == QString::fromUtf8(c.want);
                ok = ok && pass;
                QTextStream(stdout) << "stmtattest pos " << c.pos << ": "
                                    << (pass ? "PASS" : "FAIL got=[" + got + "]")
                                    << "\n";
            }
            return ok ? 0 : 1;
        }
        if(a == QStringLiteral("--comptest")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QApplication app2(argc, argv);
            CodeEditor ed;
            ed.setCompletions({ QStringLiteral("employees"),
                                QStringLiteral("emp_dept"),
                                QStringLiteral("orders") });
            const auto countFor = [&ed](const QString &sql) {
                ed.setPlainText(sql);
                QTextCursor c = ed.textCursor();
                c.movePosition(QTextCursor::End);
                ed.setTextCursor(c);
                ed.triggerCompletion();
                return ed.completionCountForTest();
            };
            /* 1. generic fallback (no schema split) */
            const int n = countFor(QStringLiteral("SELECT emp"));
            bool ok = n >= 2;
            QTextStream(stdout) << "comptest: 'SELECT emp' = " << n
                                << (ok ? "  PASS\n" : "  FAIL\n");
            /* 2. clause-aware routing */
            ed.setCompletions({});
            ed.setSchema({ QStringLiteral("employees"), QStringLiteral("emp_dept") },
                         { QStringLiteral("emp_id"), QStringLiteral("emp_name"),
                           QStringLiteral("hire_dt") });
            const int t = countFor(QStringLiteral("SELECT * FROM emp"));
            const int col = countFor(QStringLiteral("SELECT emp"));
            const int colW = countFor(QStringLiteral("SELECT * FROM x WHERE emp"));
            const bool okT = t == 2, okC = col == 2 && colW == 2;
            QTextStream(stdout) << "comptest: FROM->tables 'emp' = " << t
                                << (okT ? "  PASS" : "  FAIL")
                                << " | SELECT/WHERE->columns 'emp' = " << col
                                << "/" << colW << (okC ? "  PASS\n" : "  FAIL\n");
            return (ok && okT && okC) ? 0 : 1;
        }
        if(a.startsWith(QStringLiteral("--opentable="))) {
            const QStringList parts = a.mid(12).split(':');
            if(parts.size() == 2)
                openTableParts = qMakePair(parts[0], parts[1]);
        }
        /* --dataview=text|grid : flip the Table Data pane's view toggle */
        if(a.startsWith(QStringLiteral("--dataview=")))
            dataViewMode = a.mid(QStringLiteral("--dataview=").size());
        /* --checkrows=0,2,4 : tick those rows in the row-select column */
        if(a.startsWith(QStringLiteral("--checkrows=")))
            checkRows = a.mid(QStringLiteral("--checkrows=").size());
        /* --hexcell=row:col:deadbeef : stage a binary x'…' edit and apply */
        if(a.startsWith(QStringLiteral("--hexcell=")))
            hexCell = a.mid(QStringLiteral("--hexcell=").size());
        /* --mkobj=VIEW : open the Create <obj> editor tab */
        if(a.startsWith(QStringLiteral("--mkobj=")))
            mkObj = a.mid(QStringLiteral("--mkobj=").size());
        /* --autoconnect=host:port:user:password:db */
        if(a.startsWith(QStringLiteral("--autoconnect="))) {
            const QStringList parts = a.mid(14).split(':');
            if(parts.size() == 5) {
                autoConnect.host = parts[0];
                autoConnect.port = parts[1].toInt();
                autoConnect.user = parts[2];
                autoConnect.password = parts[3];
                autoConnect.database = parts[4];
                autoConnect.name =
                    QStringLiteral("%1@%2:%3").arg(autoConnect.user,
                                                   autoConnect.host).arg(autoConnect.port);
                doAutoConnect = true;
            }
        }
        /* --autoconnectfile=PATH.sqlite — same downstream selftest machinery
         * (--opentable=/--editcell=/--dataview=/--checkrows=/--screenshot=/
         * --dumpdb=/--copydb=) as --autoconnect=, but for the SQLite driver */
        if(a.startsWith(QStringLiteral("--autoconnectfile="))) {
            autoConnect.driverType = DriverType::Sqlite;
            autoConnect.filePath = a.mid(QStringLiteral("--autoconnectfile=").size());
            autoConnect.name = QFileInfo(autoConnect.filePath).fileName();
            doAutoConnect = true;
        }
    }

    /* headless rendering: needs to be set before QApplication starts */
    if((!screenshot.isEmpty() || !dumpPath.isEmpty() || !copyDbArg.isEmpty())
       && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("OpenYog"));
    QApplication::setOrganizationName(QStringLiteral("OpenYog"));

    /* window / taskbar icon — qt/openyog.svg rasterised into qt/resources, via the .qrc */
    QIcon appIcon;
    for(int sz : { 16, 32, 64, 256 })
        appIcon.addFile(QStringLiteral(":/resources/openyog-%1.png").arg(sz),
                        QSize(sz, sz));
    QApplication::setWindowIcon(appIcon);

    /* SQLyog-look tab bars; palette/stylesheets follow the saved theme */
    const QString theme = Theme::load();
    Theme::apply(app, theme);

    dbDriverFor(DriverType::Mysql)->libraryInit();
    int rc = 0;

    /* --delconn=NAME selftest: delete a saved connection and exit */
    if(!delConn.isEmpty()) {
        const bool existed = ConnectionStore::storedNames().contains(delConn);
        ConnectionStore::remove(delConn);
        const bool gone = !ConnectionStore::storedNames().contains(delConn);
        qInfo("delconn '%s': existed=%d removed=%d",
              qPrintable(delConn), existed, gone);
        dbDriverFor(DriverType::Mysql)->libraryShutdown();
        return (existed && gone) ? 0 : 1;
    }

    /* --dumpdb=FILE selftest (headless, no screenshot): autoconnect, dump, exit */
    if(!dumpPath.isEmpty() && doAutoConnect) {
        MainWindow w;
        rc = (w.openAndRun(autoConnect) && w.selftestDump(dumpPath)) ? 0 : 1;
        dbDriverFor(DriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --copydb=src:tgt selftest (headless): autoconnect, copy database, exit */
    if(!copyDbArg.isEmpty() && doAutoConnect) {
        const QStringList p = copyDbArg.split(':');
        MainWindow w;
        rc = (p.size() == 2 && w.openAndRun(autoConnect)
              && w.selftestCopyDb(p[0], p[1])) ? 0 : 1;
        dbDriverFor(DriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --sqlitecopydb=target[:nodata] selftest (headless): autoconnect (via
     * --autoconnectfile=), copy the open .sqlite file, exit */
    if(!sqliteCopyDbArg.isEmpty() && doAutoConnect) {
        const bool nodata = sqliteCopyDbArg.endsWith(QStringLiteral(":nodata"));
        const QString target = nodata
            ? sqliteCopyDbArg.chopped(7) : sqliteCopyDbArg;
        MainWindow w;
        rc = (w.openAndRun(autoConnect)
              && w.selftestCopySqliteFile(target, !nodata)) ? 0 : 1;
        dbDriverFor(DriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --sqlitecsvimport=file.csv:table selftest (headless): autoconnect (via
     * --autoconnectfile=), import the CSV, exit */
    if(!sqliteCsvImportArg.isEmpty() && doAutoConnect) {
        const QStringList p = sqliteCsvImportArg.split(':');
        MainWindow w;
        rc = (p.size() == 2 && w.openAndRun(autoConnect)
              && w.selftestCsvImportSqlite(p[0], p[1])) ? 0 : 1;
        dbDriverFor(DriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --schemahtmltest=out.html selftest (headless): autoconnect, build the
     * schema HTML, write it to disk, exit */
    if(!schemaHtmlArg.isEmpty() && doAutoConnect) {
        MainWindow w;
        rc = (w.openAndRun(autoConnect)
              && w.selftestSchemaHtml(schemaHtmlArg)) ? 0 : 1;
        dbDriverFor(DriverType::Mysql)->libraryShutdown();
        return rc;
    }

    if(!screenshot.isEmpty()) {
        if(shotDialog || shotCreateTable || shotIndexDlg || shotExportDlg) {
            QWidget *dlg = nullptr;
            if(shotExportDlg) {
                auto *ed = new ExportDialog(QStringLiteral("employees"),
                    QStringLiteral("employees"), 42, true);
                if(auto *cb = ed->findChild<QComboBox *>())
                    cb->setCurrentText(QStringLiteral("SQL INSERT statements"));
                dlg = ed;
            } else if(shotIndexDlg) {
                IndexDialog::IndexDef pk{ QStringLiteral("PRIMARY"),
                    { QStringLiteral("id") }, true, true };
                IndexDialog::IndexDef ix{ QStringLiteral("idx_city"),
                    { QStringLiteral("city") }, false, false };
                dlg = new IndexDialog(QStringLiteral("port_test"),
                    QStringLiteral("employees"), { pk, ix },
                    { QStringLiteral("id"), QStringLiteral("name"),
                      QStringLiteral("salary"), QStringLiteral("city") });
            } else if(shotCreateTable) {
                dlg = new CreateTableDialog(QStringLiteral("port_test"));
            } else {
                dlg = new ConnectionDialog;
                if(!dialogDriver.isEmpty()) {
                    if(auto *combo = dlg->findChild<QComboBox *>(
                           QStringLiteral("driverCombo")))
                        combo->setCurrentIndex(
                            dialogDriver.compare(QStringLiteral("sqlite"),
                                                 Qt::CaseInsensitive) == 0 ? 1 : 0);
                }
            }
            dlg->show();
            QTimer::singleShot(200, [dlg, screenshot] {
                dlg->grab().save(screenshot);
                QApplication::quit();
            });
            QApplication::exec();
            dbDriverFor(DriverType::Mysql)->libraryShutdown();
            return rc;
        }
        std::unique_ptr<MainWindow> w(new MainWindow);
        w->show();
        /* threaded execution needs more than a frame or two to land */
        QTimer::singleShot(1500, [&, w = w.get()] {
            if(doAutoConnect)
                w->openAndRun(autoConnect);
            /* open the table only after the batch's results have landed,
             * so the data grid keeps the focus */
            QTimer::singleShot(600, [&, w] {
                if(!openTableParts.first.isEmpty())
                    w->openTableData(openTableParts.first, openTableParts.second);
                QTimer::singleShot(400, [&, w] {
                    if(editRow >= 0)
                        w->editTableCell(editRow, editCol, editValue, stageOnly);
                    if(!dataViewMode.isEmpty())
                        w->setDataViewMode(dataViewMode);
                    if(!checkRows.isEmpty())
                        w->setDataViewMode(QStringLiteral("check:") + checkRows);
                    if(!hexCell.isEmpty())
                        w->setDataViewMode(QStringLiteral("hex:") + hexCell);
                    if(!mkObj.isEmpty())
                        w->openSchemaObjectTab(mkObj);
                    QTimer::singleShot(600, [w, screenshot] {
                        w->grab().save(screenshot);
                        QApplication::quit();
                    });
                });
            });
        });
        QApplication::exec();
    } else if(shotDialog) {
        ConnectionDialog dlg;
        dlg.exec();
    } else {
        MainWindow w;
        w.show();
        rc = QApplication::exec();
    }

    dbDriverFor(DriverType::Mysql)->libraryShutdown();
    return rc;
}
