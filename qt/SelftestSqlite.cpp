/* OpenYog — standalone selftests: the SQLite driver and the dialogs that write SQLite DDL, against
 * a prepared file: metadata shapes, Create/Alter Table, indexes, and the rest. Declared in
 * Selftest.h; qt/main.cpp runs one per flag. */
#include "Selftest.h"
#include "ConnectionDialog.h"
#include "ConnectionParams.h"
#include "CreateTableDialog.h"
#include "ForeignKeyDialog.h"
#include "IndexDialog.h"
#include "ExportDialog.h"
#include "SqlDumpDialog.h"
#include "SqlSplit.h"
#include "CustomFilterDialog.h"
#include "Theme.h"
#include "db/IDbDriver.h"
#include "db/IDbConnection.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTextStream>

namespace Selftest {

/* --sqlitetest=FILE — exercise the SQLite driver's canonical metadata
 * shapes (qt/db/IDbConnection.h) headlessly against a prepared SQLite
 * file; expects an (employees: INTEGER PRIMARY KEY id, a city index,
 * an inline FK to emp_dept, a BEFORE INSERT trigger; plus a view; no
 * routines/events) layout. */
int sqliteTest(int &argc, char **argv, const QString &file)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ConnectionParams cp;
    cp.driverType = SqlDriverType::Sqlite;
    cp.filePath = file;
    /* refuse a missing file up front: sqlite's create-on-open
     * would silently connect to a fresh empty database and fail
     * every shape check below (seen when a /tmp fixture was
     * wiped between sessions) */
    if(!QFileInfo::exists(cp.filePath)) {
        QTextStream(stdout) << "sqlitetest: " << cp.filePath << " does not exist\n";
        return 1;
    }
    QString connectError;
    IDbConnection *c = dbDriverFor(cp.driverType)->connect(cp, &connectError);
    if(!c) {
        QTextStream(stdout) << "sqlitetest: connect failed: " << connectError << '\n';
        return 1;
    }
    int fails = 0;
    const auto check = [&](bool ok, const QString &what) {
        if(!ok)
            ++fails;
        QTextStream(stdout) << "sqlitetest " << what << (ok ? "  PASS\n" : "  FAIL\n");
    };
    const auto findRow = [](const DbResultSet &rs, int col, const QString &val) {
        for(const QStringList &row : rs.rows)
            if(row.value(col) == val)
                return row;
        return QStringList{};
    };

    check(c->listDatabases().contains(QStringLiteral("main")), "databases contain main");

    const QStringList tbls = c->listTables(QStringLiteral("main"));
    check(tbls.contains(QStringLiteral("employees")) && tbls.contains(QStringLiteral("emp_dept")) &&
              !tbls.contains(QStringLiteral("v_employee_dept")),
          "listTables base tables");
    const QStringList views = c->listTables(QStringLiteral("main"), QStringLiteral("VIEW"));
    check(views.contains(QStringLiteral("v_employee_dept")) && views.size() == 1,
          "listTables views");

    const DbResultSet cols = c->listColumns(QStringLiteral("main"), QStringLiteral("employees"));
    const QStringList idCol = findRow(cols, 0, QStringLiteral("id"));
    check(idCol.value(1) == QStringLiteral("INTEGER") && idCol.value(2) == QStringLiteral("NO") &&
              idCol.value(3) == QStringLiteral("PRI") &&
              idCol.value(5).contains(QStringLiteral("auto_increment")),
          "listColumns id shape");
    check(findRow(cols, 0, QStringLiteral("name")).value(2) == QStringLiteral("NO") &&
              findRow(cols, 0, QStringLiteral("salary")).value(2) == QStringLiteral("YES"),
          "listColumns nullability");

    const DbResultSet ixs = c->listIndexes(QStringLiteral("main"), QStringLiteral("employees"));
    const QStringList pk = findRow(ixs, 2, QStringLiteral("PRIMARY"));
    check(pk.value(1) == QStringLiteral("0") && pk.value(4) == QStringLiteral("id"),
          "listIndexes PRIMARY (rowid alias)");
    const QStringList cityIx = findRow(ixs, 2, QStringLiteral("idx_employees_city"));
    check(cityIx.value(1) == QStringLiteral("1") && cityIx.value(4) == QStringLiteral("city"),
          "listIndexes secondary index");

    const DbResultSet fks = c->listForeignKeys(QStringLiteral("main"), QStringLiteral("employees"));
    check(!fks.rows.isEmpty() && fks.rows.first().value(1) == QStringLiteral("dept_id") &&
              fks.rows.first().value(2) == QStringLiteral("emp_dept") &&
              fks.rows.first().value(3) == QStringLiteral("id"),
          "listForeignKeys shape");

    const DbResultSet trgs =
        c->listTableTriggers(QStringLiteral("main"), QStringLiteral("employees"));
    check(trgs.rows.size() == 1 &&
              trgs.rows.first().value(0) == QStringLiteral("trg_employees_no_negative_salary") &&
              trgs.rows.first().value(1) == QStringLiteral("BEFORE") &&
              trgs.rows.first().value(2) == QStringLiteral("INSERT"),
          "listTableTriggers shape");
    check(c->listTriggers(QStringLiteral("main"))
              .contains(QStringLiteral("trg_employees_no_negative_salary")),
          "listTriggers");

    check(c->listRoutines(QStringLiteral("main")).rows.isEmpty(), "listRoutines empty on SQLite");
    check(c->listEvents(QStringLiteral("main")).isEmpty(), "listEvents empty on SQLite");

    QString ddlErr;
    check(c->showCreate(QStringLiteral("TABLE"), QStringLiteral("main"),
                        QStringLiteral("employees"), &ddlErr)
              .startsWith(QStringLiteral("CREATE TABLE")),
          "showCreate TABLE");
    check(!c->showCreate(QStringLiteral("VIEW"), QStringLiteral("main"),
                         QStringLiteral("v_employee_dept"), &ddlErr)
               .isEmpty(),
          "showCreate VIEW");
    const QString procDdl = c->showCreate(QStringLiteral("PROCEDURE"), QStringLiteral("main"),
                                          QStringLiteral("nope"), &ddlErr);
    check(procDdl.isEmpty() && !ddlErr.isEmpty(), "showCreate PROCEDURE rejected");

    check(c->sqlInsertDefaults(QStringLiteral("main"), QStringLiteral("t"))
              .contains(QStringLiteral("DEFAULT VALUES")),
          "sqlInsertDefaults shape");
    check(c->sqlFkChecks(false).startsWith(QStringLiteral("PRAGMA")), "sqlFkChecks shape");
    check(!c->supportsLimitOnUpdateDelete(), "supportsLimitOnUpdateDelete false");

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
            bool ok = c2->query(QStringLiteral("CREATE TABLE \"main\".\"dml_t\" "
                                               "(id INTEGER PRIMARY KEY, name TEXT)"),
                                nullptr, &e);
            ok = c2->query(c2->sqlInsertDefaults(QStringLiteral("main"), QStringLiteral("dml_t")),
                           nullptr, &e) &&
                 ok;
            ok = c2->query(QStringLiteral("UPDATE \"main\".\"dml_t\" SET \"name\" = 'x' "
                                          "WHERE \"id\" = 1"),
                           nullptr, &e) &&
                 ok;
            ok = c2->query(QStringLiteral("DELETE FROM \"main\".\"dml_t\" WHERE \"name\" = 'x'"),
                           nullptr, &e) &&
                 ok;
            DbResultSet rs;
            ok = c2->query(QStringLiteral("SELECT COUNT(*) FROM \"main\".\"dml_t\""), &rs, &e) &&
                 ok && rs.rows.first().value(0) == QStringLiteral("0");
            check(ok, "dml: defaults/UPDATE/DELETE round-trip");
            if(!ok)
                QTextStream(stdout) << "  last error: " << e << '\n';
            delete c2;
        }
        QFile::remove(tmp);
    }

    for(const auto &[what, ok] : queryLimitedChecks(c))
        check(ok, what);
    delete c;
    return fails == 0 ? 0 : 1;
}

/* --sqlitecreatetabletest=FILE.sqlite — drive CreateTableDialog
 * headlessly (via its objectName-tagged widgets, not its private
 * grid-column enum) against a real SQLite file: Create Table with
 * an autoincrement PK plus a plain column, then Alter Table
 * renaming/adding columns, confirming a genuinely unsupported
 * change (retyping an existing column) is flagged via
 * alterLimitation() rather than silently dropped or sent as SQL
 * that would fail outright. Grid columns, per CreateTableDialog.h's
 * private `enum Col`: Name=0 Type=1 Len=2 Default=3 PK=4 NotNull=5
 * Unsigned=6 Auto=7 Comment=8. */
int sqliteCreateTableTest(int &argc, char **argv, const QString &path)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QFile::remove(path);
    ConnectionParams cp;
    cp.driverType = SqlDriverType::Sqlite;
    cp.filePath = path;
    QString connectError;
    IDbConnection *c = dbDriverFor(cp.driverType)->connect(cp, &connectError);
    if(!c) {
        QTextStream(stdout) << "sqlitecreatetabletest: connect failed: " << connectError << '\n';
        return 1;
    }
    int fails = 0;
    const auto check = [&](bool ok, const QString &what) {
        if(!ok)
            ++fails;
        QTextStream(stdout) << "sqlitecreatetabletest " << what << (ok ? "  PASS\n" : "  FAIL\n");
    };
    const auto checkBoxAt = [](QTableWidget *g, int r, int col) {
        return g->cellWidget(r, col) ? g->cellWidget(r, col)->findChild<QCheckBox *>() : nullptr;
    };

    /* ---- CREATE mode: id (seeded PK+auto) + a NOT NULL name col */
    auto *cdlg = new CreateTableDialog(QString(), nullptr, SqlDriverType::Sqlite);
    auto *cname = cdlg->findChild<QLineEdit *>(QStringLiteral("tableName"));
    auto *cgrid = cdlg->findChild<QTableWidget *>(QStringLiteral("columnGrid"));
    auto *caddBtn = cdlg->findChild<QPushButton *>(QStringLiteral("addColumnBtn"));
    cname->setText(QStringLiteral("t1"));
    caddBtn->click();
    cgrid->item(1, 0)->setText(QStringLiteral("name"));
    if(auto *tb = qobject_cast<QComboBox *>(cgrid->cellWidget(1, 1)))
        tb->setCurrentText(QStringLiteral("TEXT"));
    if(auto *cb = checkBoxAt(cgrid, 1, 5))
        cb->setChecked(true); /* NOT NULL */

    const QString createSql = cdlg->buildSql();
    check(createSql.contains(QStringLiteral("INTEGER PRIMARY KEY AUTOINCREMENT")),
          "create: id column is INTEGER PRIMARY KEY AUTOINCREMENT");
    check(!createSql.contains(QStringLiteral("ENGINE")) &&
              !createSql.contains(QStringLiteral("CHARSET")),
          "create: no ENGINE/CHARSET clause");
    QString stmtErr;
    check(c->query(createSql, nullptr, &stmtErr), "create: statement executes");
    if(!stmtErr.isEmpty())
        QTextStream(stdout) << "  error: " << stmtErr << " sql=" << createSql << '\n';
    check(c->query(QStringLiteral("INSERT INTO \"t1\" (\"name\") VALUES ('a'), ('b')"), nullptr,
                   &stmtErr),
          "create: insert into new table (autoincrement works)");
    DbResultSet crs;
    c->query(QStringLiteral("SELECT \"id\", \"name\" FROM \"t1\" ORDER BY \"id\""), &crs, nullptr);
    check(crs.rows.size() == 2 && crs.rows.at(0).value(0) == QStringLiteral("1") &&
              crs.rows.at(1).value(0) == QStringLiteral("2"),
          "create: autoincrement produced 1, 2");
    delete cdlg;

    /* ---- ALTER mode: rename a column + add a new one (supported) */
    QList<CreateTableDialog::ColumnDef> cols;
    {
        CreateTableDialog::ColumnDef idc;
        idc.name = QStringLiteral("id");
        idc.type = QStringLiteral("INTEGER");
        idc.pk = true;
        idc.notNull = true;
        idc.autoInc = true;
        CreateTableDialog::ColumnDef namec;
        namec.name = QStringLiteral("name");
        namec.type = QStringLiteral("TEXT");
        namec.notNull = true;
        cols << idc << namec;
    }
    auto *adlg = new CreateTableDialog(QString(), QStringLiteral("t1"), cols, QString(), QString(),
                                       nullptr, SqlDriverType::Sqlite);
    auto *agrid = adlg->findChild<QTableWidget *>(QStringLiteral("columnGrid"));
    auto *aaddBtn = adlg->findChild<QPushButton *>(QStringLiteral("addColumnBtn"));
    agrid->item(1, 0)->setText(QStringLiteral("full_name")); /* rename */
    aaddBtn->click();
    agrid->item(2, 0)->setText(QStringLiteral("note"));
    if(auto *tb = qobject_cast<QComboBox *>(agrid->cellWidget(2, 1)))
        tb->setCurrentText(QStringLiteral("TEXT"));

    const QString alterSql1 = adlg->buildSql();
    check(alterSql1.contains(QStringLiteral("RENAME COLUMN")) &&
              alterSql1.contains(QStringLiteral("ADD COLUMN")),
          "alter: rename + add column both emitted");
    check(adlg->alterLimitation().isEmpty(), "alter: no limitation reported for rename+add");
    /* sqlite3_prepare_v2 only ever runs the FIRST statement in a
     * string (see SqliteConnection::runBuffered) — split, matching
     * what ConnectionTab::execDdl() now does for the real UI path,
     * rather than the single-query() call that would silently drop
     * every statement after the first here in the test too */
    bool alterOk = true;
    for(const QString &s : splitStatements(alterSql1, SqlDriverType::Sqlite)) {
        if(s.trimmed().isEmpty())
            continue;
        if(!c->query(s, nullptr, &stmtErr)) {
            alterOk = false;
            break;
        }
    }
    check(alterOk, "alter: statement(s) execute");
    if(!stmtErr.isEmpty())
        QTextStream(stdout) << "  error: " << stmtErr << " sql=" << alterSql1 << '\n';
    DbResultSet ars;
    c->query(QStringLiteral("SELECT \"full_name\", \"note\" FROM \"t1\""), &ars, nullptr);
    check(!ars.headers.isEmpty(), "alter: both full_name and note columns exist");
    delete adlg;

    /* ---- ALTER mode: an unsupported change (retype an existing
     * column) must be flagged, not silently dropped or sent broken */
    QList<CreateTableDialog::ColumnDef> cols2;
    {
        CreateTableDialog::ColumnDef idc;
        idc.name = QStringLiteral("id");
        idc.type = QStringLiteral("INTEGER");
        idc.pk = true;
        idc.notNull = true;
        idc.autoInc = true;
        CreateTableDialog::ColumnDef fnc;
        fnc.name = QStringLiteral("full_name");
        fnc.type = QStringLiteral("TEXT");
        fnc.notNull = true;
        CreateTableDialog::ColumnDef notec;
        notec.name = QStringLiteral("note");
        notec.type = QStringLiteral("TEXT");
        cols2 << idc << fnc << notec;
    }
    auto *bdlg = new CreateTableDialog(QString(), QStringLiteral("t1"), cols2, QString(), QString(),
                                       nullptr, SqlDriverType::Sqlite);
    auto *bgrid = bdlg->findChild<QTableWidget *>(QStringLiteral("columnGrid"));
    if(auto *tb = qobject_cast<QComboBox *>(bgrid->cellWidget(1, 1)))
        tb->setCurrentText(QStringLiteral("INTEGER")); /* full_name TEXT -> INTEGER */
    const QString alterSql2 = bdlg->buildSql();
    check(alterSql2.isEmpty(), "alter: retype-only change produces no SQL");
    check(!bdlg->alterLimitation().isEmpty(),
          "alter: retype-only change flagged via alterLimitation()");
    delete bdlg;

    /* ---- ALTER mode: column reorder via Move Up/Down. SQLite
     * can't reposition columns at all -> alterLimitation() flags
     * it while a simultaneous rename still goes through */
    auto *rdlg = new CreateTableDialog(QString(), QStringLiteral("t1"), cols2, QString(), QString(),
                                       nullptr, SqlDriverType::Sqlite);
    auto *rgrid = rdlg->findChild<QTableWidget *>(QStringLiteral("columnGrid"));
    rgrid->setCurrentCell(2, 0); /* "note" */
    rdlg->findChild<QPushButton *>(QStringLiteral("moveUpBtn"))->click();
    check(rgrid->item(1, 0)->text() == QStringLiteral("note"),
          "reorder: Move Up swaps the grid rows");
    const QString alterSql3 = rdlg->buildSql();
    check(alterSql3.isEmpty(), "reorder: SQLite reorder produces no SQL");
    check(rdlg->alterLimitation().contains(QStringLiteral("reorder")),
          "reorder: SQLite flags it via alterLimitation()");
    /* rename alongside the (unsupported) reorder still emits */
    rgrid->item(2, 0)->setText(QStringLiteral("memo"));
    const QString alterSql4 = rdlg->buildSql();
    check(alterSql4.contains(QStringLiteral("RENAME COLUMN")),
          "reorder+rename: rename still emitted");
    delete rdlg;

    /* ---- the MySQL dialect of the same reorder emits AFTER clauses
     * (pure string building — buildSql never touches a connection,
     * so the SQLite conn above can host the dialog while the DDL
     * is MySQL's). Swap full_name/note: note moves up between id
     * and full_name (AFTER `id`), full_name moves down (AFTER
     * `note`); id itself hasn't moved and emits nothing. */
    auto *mdlg = new CreateTableDialog(QString(), QStringLiteral("t1"), cols2, QString(), QString(),
                                       nullptr, SqlDriverType::Mysql);
    auto *mgrid = mdlg->findChild<QTableWidget *>(QStringLiteral("columnGrid"));
    mgrid->setCurrentCell(2, 0); /* "note" */
    mdlg->findChild<QPushButton *>(QStringLiteral("moveUpBtn"))->click();
    const QString mysqlReorder = mdlg->buildSql();
    check(mysqlReorder.contains(QStringLiteral("CHANGE COLUMN `note`")) &&
              mysqlReorder.contains(QStringLiteral("AFTER `id`")),
          "reorder mysql: moved-up column carries AFTER id");
    check(mysqlReorder.contains(QStringLiteral("CHANGE COLUMN `full_name`")) &&
              mysqlReorder.contains(QStringLiteral("AFTER `note`")),
          "reorder mysql: moved-down column carries AFTER note");
    /* moving id itself down makes full_name the top column — the
     * one case that emits FIRST */
    auto *fdlg = new CreateTableDialog(QString(), QStringLiteral("t1"), cols2, QString(), QString(),
                                       nullptr, SqlDriverType::Mysql);
    auto *fgrid = fdlg->findChild<QTableWidget *>(QStringLiteral("columnGrid"));
    fgrid->setCurrentCell(0, 0); /* "id" */
    fdlg->findChild<QPushButton *>(QStringLiteral("moveDownBtn"))->click();
    const QString mysqlFirst = fdlg->buildSql();
    check(mysqlFirst.contains(QStringLiteral("FIRST")) &&
              mysqlFirst.contains(QStringLiteral("CHANGE COLUMN `full_name`")),
          "reorder mysql: column moved to the top carries FIRST");
    delete fdlg;
    /* untouched-position edits must NOT grow stray FIRST/AFTER */
    auto *ndlg = new CreateTableDialog(QString(), QStringLiteral("t1"), cols2, QString(), QString(),
                                       nullptr, SqlDriverType::Mysql);
    auto *ngrid = ndlg->findChild<QTableWidget *>(QStringLiteral("columnGrid"));
    ngrid->item(2, 0)->setText(QStringLiteral("memo")); /* plain rename, no move */
    const QString mysqlRename = ndlg->buildSql();
    check(mysqlRename.contains(QStringLiteral("CHANGE COLUMN")) &&
              !mysqlRename.contains(QStringLiteral("AFTER")) &&
              !mysqlRename.contains(QStringLiteral("FIRST")),
          "reorder mysql: same-position rename emits no AFTER/FIRST");
    delete ndlg;
    delete mdlg;

    delete c;
    return fails == 0 ? 0 : 1;
}

/* --sqliteindextest=FILE.sqlite — drive IndexDialog headlessly (via
 * its objectName-tagged widgets) against a real SQLite file: add a
 * new index and drop an existing one, confirming both come out as
 * standalone CREATE INDEX/DROP INDEX statements (SQLite has no
 * ALTER TABLE ADD/DROP INDEX clause, same as Postgres) and that
 * ConnectionTab::execDdl()'s split-per-statement fix actually
 * applies every one of them, not just the first. */
int sqliteIndexTest(int &argc, char **argv, const QString &path)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QFile::remove(path);
    ConnectionParams cp;
    cp.driverType = SqlDriverType::Sqlite;
    cp.filePath = path;
    QString connectError;
    IDbConnection *c = dbDriverFor(cp.driverType)->connect(cp, &connectError);
    if(!c) {
        QTextStream(stdout) << "sqliteindextest: connect failed: " << connectError << '\n';
        return 1;
    }
    int fails = 0;
    const auto check = [&](bool ok, const QString &what) {
        if(!ok)
            ++fails;
        QTextStream(stdout) << "sqliteindextest " << what << (ok ? "  PASS\n" : "  FAIL\n");
    };

    QString stmtErr;
    check(c->query(QStringLiteral("CREATE TABLE \"t1\" (\"id\" INTEGER PRIMARY KEY, "
                                  "\"city\" TEXT, \"name\" TEXT)"),
                   nullptr, &stmtErr),
          "setup: create table");
    check(c->query(QStringLiteral("CREATE INDEX \"idx_old\" ON \"t1\" (\"name\")"), nullptr,
                   &stmtErr),
          "setup: create pre-existing index");

    IndexDialog::IndexDef pk{QStringLiteral("PRIMARY"), {QStringLiteral("id")}, true, true};
    IndexDialog::IndexDef old{QStringLiteral("idx_old"), {QStringLiteral("name")}, false, false};
    auto *dlg =
        new IndexDialog(QString(), QStringLiteral("t1"), {pk, old},
                        {QStringLiteral("id"), QStringLiteral("city"), QStringLiteral("name")},
                        nullptr, SqlDriverType::Sqlite);
    auto *grid = dlg->findChild<QTableWidget *>(QStringLiteral("indexGrid"));
    auto *removeBtn = dlg->findChild<QPushButton *>(QStringLiteral("removeSelectedBtn"));
    auto *newName = dlg->findChild<QLineEdit *>(QStringLiteral("newIndexName"));
    auto *newCols = dlg->findChild<QListWidget *>(QStringLiteral("newIndexCols"));
    auto *addBtn = dlg->findChild<QPushButton *>(QStringLiteral("addIndexBtn"));

    /* drop idx_old (row 1: row 0 is PRIMARY) */
    grid->setCurrentCell(1, 0);
    removeBtn->click();
    /* add a new index on city */
    newName->setText(QStringLiteral("idx_city"));
    newCols->item(1)->setCheckState(Qt::Checked); /* "city" */
    addBtn->click();

    const QString sql = dlg->buildSql();
    check(sql.contains(QStringLiteral("DROP INDEX")) && sql.contains(QStringLiteral("idx_old")),
          "drop: idx_old emitted as standalone DROP INDEX");
    check(sql.contains(QStringLiteral("CREATE")) && sql.contains(QStringLiteral("INDEX")) &&
              sql.contains(QStringLiteral("idx_city")),
          "add: idx_city emitted as standalone CREATE INDEX");
    check(!sql.contains(QStringLiteral("ALTER TABLE")),
          "no ALTER TABLE clause used (SQLite has none for indexes)");
    delete dlg;

    /* apply exactly like ConnectionTab::execDdl() does for SQLite:
     * split on ';' and run each statement separately */
    bool applyOk = true;
    for(const QString &s : splitStatements(sql, SqlDriverType::Sqlite)) {
        if(s.trimmed().isEmpty())
            continue;
        if(!c->query(s, nullptr, &stmtErr)) {
            applyOk = false;
            break;
        }
    }
    check(applyOk, "both statements execute");
    if(!applyOk)
        QTextStream(stdout) << "  error: " << stmtErr << '\n';

    DbResultSet ixs;
    c->query(QStringLiteral("SELECT name FROM sqlite_master WHERE type='index' AND tbl_name='t1'"),
             &ixs, nullptr);
    QStringList names;
    for(const QStringList &row : ixs.rows)
        names << row.value(0);
    check(names.contains(QStringLiteral("idx_city")) && !names.contains(QStringLiteral("idx_old")),
          "final state: idx_city present, idx_old gone");

    /* composite (multi-column) index: check two columns, assert
     * both come out comma-joined in the CREATE INDEX column list —
     * the SQL side always supported this; this pins the input path */
    {
        auto *dlg2 =
            new IndexDialog(QString(), QStringLiteral("t1"), {},
                            {QStringLiteral("id"), QStringLiteral("city"), QStringLiteral("name")},
                            nullptr, SqlDriverType::Sqlite);
        auto *newName2 = dlg2->findChild<QLineEdit *>(QStringLiteral("newIndexName"));
        auto *newCols2 = dlg2->findChild<QListWidget *>(QStringLiteral("newIndexCols"));
        newName2->setText(QStringLiteral("idx_cn"));
        newCols2->item(1)->setCheckState(Qt::Checked); /* "city" */
        newCols2->item(2)->setCheckState(Qt::Checked); /* "name" */
        dlg2->findChild<QPushButton *>(QStringLiteral("addIndexBtn"))->click();
        const QString sql2 = dlg2->buildSql();
        check(sql2.contains(QStringLiteral("idx_cn")) &&
                  sql2.contains(QStringLiteral("(`city`, `name`)")),
              "composite: both columns comma-joined in CREATE INDEX");
        delete dlg2;
    }

    delete c;
    return fails == 0 ? 0 : 1;
}

/* --sqlitemisctest=FILE.sqlite — three more Postgres-retrofit
 * regressions caught by the same UI-gating audit: Rename Table
 * (was MySQL-only RENAME TABLE syntax), Duplicate Table structure
 * (was MySQL/SQLite "CREATE TABLE ... LIKE ...", but SQLite has no
 * LIKE clause at all), and ForeignKeyDialog correctly refusing to
 * emit SQL for an operation SQLite's ALTER TABLE can't do (add/drop
 * a foreign key on an existing table needs a full rebuild). */
int sqliteMiscTest(int &argc, char **argv, const QString &path)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QFile::remove(path);
    ConnectionParams cp;
    cp.driverType = SqlDriverType::Sqlite;
    cp.filePath = path;
    QString connectError;
    IDbConnection *c = dbDriverFor(cp.driverType)->connect(cp, &connectError);
    if(!c) {
        QTextStream(stdout) << "sqlitemisctest: connect failed: " << connectError << '\n';
        return 1;
    }
    int fails = 0;
    const auto check = [&](bool ok, const QString &what) {
        if(!ok)
            ++fails;
        QTextStream(stdout) << "sqlitemisctest " << what << (ok ? "  PASS\n" : "  FAIL\n");
    };

    QString stmtErr;
    check(c->query(QStringLiteral("CREATE TABLE \"employees\" (\"id\" INTEGER PRIMARY KEY "
                                  "AUTOINCREMENT, \"name\" TEXT NOT NULL, "
                                  "\"salary\" REAL DEFAULT 0)"),
                   nullptr, &stmtErr),
          "setup: create table");
    check(c->query(QStringLiteral("INSERT INTO \"employees\" (\"name\") VALUES ('a')"), nullptr,
                   &stmtErr),
          "setup: insert a row");

    /* --- Rename Table: ALTER TABLE ... RENAME TO ... --- */
    check(c->query(QStringLiteral("ALTER TABLE %1 RENAME TO %2")
                       .arg(c->qualify(QString(), QStringLiteral("employees")),
                            c->quoteIdent(QStringLiteral("staff"))),
                   nullptr, &stmtErr),
          "rename: ALTER TABLE ... RENAME TO applies");
    DbResultSet trs;
    c->query(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'"), &trs, nullptr);
    QStringList tnames;
    for(const QStringList &row : trs.rows)
        tnames << row.value(0);
    check(tnames.contains(QStringLiteral("staff")) && !tnames.contains(QStringLiteral("employees")),
          "rename: staff exists, employees gone");

    /* --- Duplicate Table (structure): retarget showCreate()'s own
     * DDL text to the new name, since SQLite has no LIKE clause */
    QString err;
    QString ddl = c->showCreate(QStringLiteral("TABLE"), QString(), QStringLiteral("staff"), &err);
    static const QRegularExpression nameRe(
        QStringLiteral("^(CREATE\\s+TABLE\\s+(?:IF\\s+NOT\\s+EXISTS\\s+)?)"
                       "(?:\"[^\"]+\"|`[^`]+`|\\[[^\\]]+\\]|\\w+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QString dst = c->qualify(QString(), QStringLiteral("staff_copy"));
    ddl.replace(nameRe, QStringLiteral("\\1") + dst);
    check(!ddl.isEmpty() && ddl.contains(QStringLiteral("staff_copy")) &&
              !ddl.contains(QStringLiteral("\"staff\"")),
          "duplicate: DDL retargeted to staff_copy, no leftover staff reference");
    check(c->query(ddl, nullptr, &stmtErr), "duplicate: retargeted DDL executes");
    if(!stmtErr.isEmpty() && !stmtErr.startsWith(QStringLiteral("OK")))
        QTextStream(stdout) << "  error: " << stmtErr << " ddl=" << ddl << '\n';
    check(c->query(QStringLiteral(
                       "INSERT INTO \"staff_copy\" (\"name\") SELECT \"name\" FROM \"staff\""),
                   nullptr, &stmtErr),
          "duplicate: data copy into the new table works");
    DbResultSet drs;
    c->query(QStringLiteral("SELECT \"id\", \"name\", \"salary\" FROM \"staff_copy\""), &drs,
             nullptr);
    check(drs.rows.size() == 1 && drs.rows.first().value(1) == QStringLiteral("a"),
          "duplicate: copy has the same column shape and data");

    /* --- Foreign Keys: adding one to an existing SQLite table must
     * be refused (via limitation()), not sent as broken SQL. Driven
     * through the checklist pickers (local + referenced columns,
     * nth-checked ↔ nth-checked pairing), composite here: two
     * columns each way. The ref checklist is populated live from
     * the connection when m_refTable's selection changes — same
     * conn as everything else in this test. */
    const auto checkFkItem = [](QListWidget *lw, const QString &name) {
        const auto hits = lw->findItems(name, Qt::MatchExactly);
        if(!hits.isEmpty())
            hits.first()->setCheckState(Qt::Checked);
    };
    ForeignKeyDialog fkdlg(QString(), QStringLiteral("staff"), {},
                           {QStringLiteral("id"), QStringLiteral("name")},
                           {QStringLiteral("staff")}, c, nullptr, SqlDriverType::Sqlite);
    checkFkItem(fkdlg.findChild<QListWidget *>(QStringLiteral("localCols")), QStringLiteral("id"));
    checkFkItem(fkdlg.findChild<QListWidget *>(QStringLiteral("localCols")),
                QStringLiteral("name"));
    checkFkItem(fkdlg.findChild<QListWidget *>(QStringLiteral("refCols")), QStringLiteral("id"));
    checkFkItem(fkdlg.findChild<QListWidget *>(QStringLiteral("refCols")), QStringLiteral("name"));
    fkdlg.findChild<QPushButton *>(QStringLiteral("addFkBtn"))->click();
    const QString fkSql = fkdlg.buildSql();
    check(fkSql.isEmpty(), "fk: SQLite add-FK produces no SQL");
    check(!fkdlg.limitation().isEmpty(), "fk: SQLite add-FK flagged via limitation() instead");

    /* same checklist-driven composite input, MySQL dialect — pure
     * string building (buildSql never touches the conn), so the
     * SQLite connection above can supply the column lists while
     * the generated DDL is asserted to comma-join BOTH column
     * lists of the composite key */
    ForeignKeyDialog fkmysql(QString(), QStringLiteral("staff"), {},
                             {QStringLiteral("id"), QStringLiteral("name")},
                             {QStringLiteral("staff")}, c, nullptr, SqlDriverType::Mysql);
    checkFkItem(fkmysql.findChild<QListWidget *>(QStringLiteral("localCols")),
                QStringLiteral("id"));
    checkFkItem(fkmysql.findChild<QListWidget *>(QStringLiteral("localCols")),
                QStringLiteral("name"));
    checkFkItem(fkmysql.findChild<QListWidget *>(QStringLiteral("refCols")), QStringLiteral("id"));
    checkFkItem(fkmysql.findChild<QListWidget *>(QStringLiteral("refCols")),
                QStringLiteral("name"));
    fkmysql.findChild<QPushButton *>(QStringLiteral("addFkBtn"))->click();
    const QString fkMySql = fkmysql.buildSql();
    check(fkMySql.contains(QStringLiteral("FOREIGN KEY (`id`, `name`)")) &&
              fkMySql.contains(QStringLiteral("REFERENCES `staff` (`id`, `name`)")),
          "fk: composite key comma-joins both column lists");

    delete c;
    return fails == 0 ? 0 : 1;
}

} // namespace Selftest
