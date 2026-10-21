/* OpenYog — standalone selftests: the PostgreSQL driver's metadata shapes and the schema SQL the
 * MySQL/PostgreSQL drivers generate, against a live server.
 * Declared in Selftest.h; qt/main.cpp runs one per flag. */
#include "Selftest.h"
#include "ConnectionParams.h"
#include "ForeignKeyDialog.h"
#include "SqlDumpDialog.h"
#include "SchemaSql.h"
#include "SqlSplit.h"
#include "Theme.h"
#include "db/IDbDriver.h"
#include "db/IDbConnection.h"

#include <QApplication>
#include <QTextStream>

namespace Selftest {

/* --pgtest=host:port:user:pw:db — exercise the PostgreSQL driver's
 * canonical metadata shapes against a live server prepared with the
 * same layout as the --sqlitetest fixture (employees/emp_dept/
 * v_employee_dept, a city index, an FK, a BEFORE INSERT trigger). */
int pgTest(int &argc, char **argv, const QString &server)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QStringList p = server.split(':');
    if(p.size() != 5) {
        QTextStream(stdout) << "pgtest: need host:port:user:pw:db\n";
        return 2;
    }
    ConnectionParams cp;
    cp.driverType = SqlDriverType::Postgres;
    cp.host = p[0];
    cp.port = p[1].toUInt();
    cp.user = p[2];
    cp.password = p[3];
    cp.database = p[4];
    QString connectError;
    IDbConnection *c = dbDriverFor(cp.driverType)->connect(cp, &connectError);
    if(!c) {
        QTextStream(stdout) << "pgtest: connect failed: " << connectError << '\n';
        return 1;
    }
    int fails = 0;
    const auto check = [&](bool ok, const QString &what) {
        if(!ok)
            ++fails;
        QTextStream(stdout) << "pgtest " << what << (ok ? "  PASS\n" : "  FAIL\n");
    };
    const auto findRow = [](const DbResultSet &rs, int col, const QString &val) {
        for(const QStringList &row : rs.rows)
            if(row.value(col) == val)
                return row;
        return QStringList{};
    };

    check(c->listDatabases().contains(QStringLiteral("public")), "schemas contain public");

    const QStringList tbls = c->listTables(QStringLiteral("public"));
    check(tbls.contains(QStringLiteral("employees")) && tbls.contains(QStringLiteral("emp_dept")) &&
              !tbls.contains(QStringLiteral("v_employee_dept")),
          "listTables base tables");
    const QStringList views = c->listTables(QStringLiteral("public"), QStringLiteral("VIEW"));
    check(views.contains(QStringLiteral("v_employee_dept")), "listTables views");

    const DbResultSet cols = c->listColumns(QStringLiteral("public"), QStringLiteral("employees"));
    const QStringList idCol = findRow(cols, 0, QStringLiteral("id"));
    check(idCol.value(2) == QStringLiteral("NO") && idCol.value(3) == QStringLiteral("PRI"),
          "listColumns id shape");
    check(findRow(cols, 0, QStringLiteral("name")).value(2) == QStringLiteral("NO") &&
              findRow(cols, 0, QStringLiteral("salary")).value(2) == QStringLiteral("YES"),
          "listColumns nullability");

    const DbResultSet ixs = c->listIndexes(QStringLiteral("public"), QStringLiteral("employees"));
    const QStringList pk = findRow(ixs, 2, QStringLiteral("PRIMARY"));
    check(pk.value(1) == QStringLiteral("0") && pk.value(4) == QStringLiteral("id"),
          "listIndexes PRIMARY");
    const QStringList cityIx = findRow(ixs, 2, QStringLiteral("idx_employees_city"));
    check(cityIx.value(1) == QStringLiteral("1") && cityIx.value(4) == QStringLiteral("city"),
          "listIndexes secondary index");

    const DbResultSet fks =
        c->listForeignKeys(QStringLiteral("public"), QStringLiteral("employees"));
    check(!fks.rows.isEmpty() && fks.rows.first().value(1) == QStringLiteral("dept_id") &&
              fks.rows.first().value(2) == QStringLiteral("emp_dept") &&
              fks.rows.first().value(3) == QStringLiteral("id"),
          "listForeignKeys shape");

    const DbResultSet trgs =
        c->listTableTriggers(QStringLiteral("public"), QStringLiteral("employees"));
    check(trgs.rows.size() == 1 &&
              trgs.rows.first().value(0) == QStringLiteral("trg_employees_no_negative_salary") &&
              trgs.rows.first().value(1) == QStringLiteral("BEFORE") &&
              trgs.rows.first().value(2) == QStringLiteral("INSERT"),
          "listTableTriggers shape");
    check(c->listTriggers(QStringLiteral("public"))
              .contains(QStringLiteral("trg_employees_no_negative_salary")),
          "listTriggers");

    const DbResultSet routines = c->listRoutines(QStringLiteral("public"));
    check(!findRow(routines, 0, QStringLiteral("trg_no_negative_salary")).isEmpty(),
          "listRoutines contains trigger function");
    check(c->listEvents(QStringLiteral("public")).isEmpty(), "listEvents empty on PostgreSQL");

    QString ddlErr;
    check(c->showCreate(QStringLiteral("TABLE"), QStringLiteral("public"),
                        QStringLiteral("employees"), &ddlErr)
              .startsWith(QStringLiteral("CREATE TABLE")),
          "showCreate TABLE");
    check(c->showCreate(QStringLiteral("VIEW"), QStringLiteral("public"),
                        QStringLiteral("v_employee_dept"), &ddlErr)
              .contains(QStringLiteral("CREATE VIEW")),
          "showCreate VIEW");
    check(c->showCreate(QStringLiteral("TRIGGER"), QStringLiteral("public"),
                        QStringLiteral("trg_employees_no_negative_salary"), &ddlErr)
              .contains(QStringLiteral("CREATE TRIGGER")),
          "showCreate TRIGGER");
    check(c->showCreate(QStringLiteral("FUNCTION"), QStringLiteral("public"),
                        QStringLiteral("trg_no_negative_salary"), &ddlErr)
              .contains(QStringLiteral("FUNCTION")),
          "showCreate FUNCTION");
    const QString evtDdl = c->showCreate(QStringLiteral("EVENT"), QStringLiteral("public"),
                                         QStringLiteral("nope"), &ddlErr);
    check(evtDdl.isEmpty() && !ddlErr.isEmpty(), "showCreate EVENT rejected");

    check(c->sqlInsertDefaults(QStringLiteral("public"), QStringLiteral("t"))
              .contains(QStringLiteral("DEFAULT VALUES")),
          "sqlInsertDefaults shape");
    check(c->sqlFkChecks(false).contains(QStringLiteral("session_replication_role")),
          "sqlFkChecks shape");
    check(!c->supportsLimitOnUpdateDelete(), "supportsLimitOnUpdateDelete false");

    /* DML round-trip on a throwaway table, mirroring the SQLite/MySQL
     * selftests' shapes (quoteIdent'd, no LIMIT on UPDATE/DELETE,
     * DEFAULT VALUES insert) */
    {
        QString e;
        c->query(QStringLiteral("DROP TABLE IF EXISTS dml_t"), nullptr, nullptr);
        bool ok = c->query(QStringLiteral("CREATE TABLE dml_t (id SERIAL PRIMARY KEY, name TEXT)"),
                           nullptr, &e);
        ok = c->query(c->sqlInsertDefaults(QStringLiteral("public"), QStringLiteral("dml_t")),
                      nullptr, &e) &&
             ok;
        ok = c->query(QStringLiteral("UPDATE \"dml_t\" SET \"name\" = 'x' WHERE \"id\" = 1"),
                      nullptr, &e) &&
             ok;
        ok = c->query(QStringLiteral("DELETE FROM \"dml_t\" WHERE \"name\" = 'x'"), nullptr, &e) &&
             ok;
        DbResultSet rs;
        ok = c->query(QStringLiteral("SELECT COUNT(*) FROM \"dml_t\""), &rs, &e) && ok &&
             rs.rows.first().value(0) == QStringLiteral("0");
        check(ok, "dml: defaults/UPDATE/DELETE round-trip");
        if(!ok)
            QTextStream(stdout) << "  last error: " << e << '\n';
        c->query(QStringLiteral("DROP TABLE IF EXISTS dml_t"), nullptr, nullptr);
    }

    /* Session Idle Timeout / Keep-Alive: a second connection with
     * both set, checking the server actually applied the GUC (idle
     * timeout) and that libpq accepted the keepalive conninfo
     * options (accepted, not observably testable from SQL — a
     * bad option value would have failed PQconnectdb outright,
     * which connectOk below already checks). */
    {
        ConnectionParams cp2 = cp;
        cp2.idleTimeoutSecs = 45;
        cp2.keepAliveSecs = 30;
        QString err2;
        IDbConnection *c2 = dbDriverFor(cp2.driverType)->connect(cp2, &err2);
        check(c2 != nullptr, "idle timeout/keepalive: connect with options set");
        if(c2) {
            DbResultSet rs;
            const bool queried =
                c2->query(QStringLiteral("SHOW idle_session_timeout"), &rs, nullptr);
            check(queried && !rs.rows.isEmpty() &&
                      rs.rows.first().value(0) == QStringLiteral("45s"),
                  "idle timeout: SET idle_session_timeout applied");
            delete c2;
        }
    }

    delete c;
    return fails == 0 ? 0 : 1;
}

/* --pgschematest=host:port:user:pw:db — the same create/alter/drop
 * round-trip as --schematest=, but for SchemaSql's PostgreSQL
 * branches (dollar-quoted bodies, CREATE OR REPLACE alters, the
 * trigger-needs-a-function two-statement template, DROP TRIGGER's
 * "ON table" pulled from the DDL). No EVENT case — PostgreSQL has
 * no equivalent, ConnectionTab guards it before ever calling in. */
int pgSchemaTest(int &argc, char **argv, const QString &server)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QStringList p = server.split(':');
    if(p.size() != 5) {
        QTextStream(stdout) << "pgschematest: need host:port:user:pw:db\n";
        return 2;
    }
    ConnectionParams cp;
    cp.driverType = SqlDriverType::Postgres;
    cp.host = p[0];
    cp.port = p[1].toUInt();
    cp.user = p[2];
    cp.password = p[3];
    cp.database = p[4];
    QString connectError;
    IDbConnection *c = dbDriverFor(cp.driverType)->connect(cp, &connectError);
    if(!c) {
        QTextStream(stdout) << "pgschematest: connect failed: " << connectError << '\n';
        return 1;
    }
    const QString db = QStringLiteral("public");
    auto run = [&](const QString &sql) {
        QString err;
        const bool ok = c->query(sql, nullptr, &err);
        if(!ok)
            QTextStream(stdout) << "  [run failed] " << sql.left(200) << "\n  -> " << err << '\n';
        return ok;
    };
    auto exists = [&](const QString &q) {
        DbResultSet rs;
        if(!c->query(q, &rs, nullptr))
            return -1;
        return rs.rows.isEmpty() ? 0 : rs.rows.first().value(0).toInt();
    };
    run(QStringLiteral("DROP TABLE IF EXISTS oy_schema_test_t"));
    run(QStringLiteral("CREATE TABLE oy_schema_test_t (id INT)"));

    struct Case
    {
        QString kw, name, iq;
    };
    const QList<Case> cases = {
        {QStringLiteral("VIEW"), QStringLiteral("oy_st_view"),
         QStringLiteral("SELECT COUNT(*) FROM information_schema.views "
                        "WHERE table_schema='%1' AND table_name='oy_st_view'")
             .arg(db)},
        {QStringLiteral("PROCEDURE"), QStringLiteral("oy_st_proc"),
         QStringLiteral("SELECT COUNT(*) FROM information_schema.routines "
                        "WHERE routine_schema='%1' AND routine_name='oy_st_proc' "
                        "AND routine_type='PROCEDURE'")
             .arg(db)},
        {QStringLiteral("FUNCTION"), QStringLiteral("oy_st_func"),
         QStringLiteral("SELECT COUNT(*) FROM information_schema.routines "
                        "WHERE routine_schema='%1' AND routine_name='oy_st_func' "
                        "AND routine_type='FUNCTION'")
             .arg(db)},
        {QStringLiteral("TRIGGER"), QStringLiteral("oy_st_trg"),
         QStringLiteral("SELECT COUNT(*) FROM information_schema.triggers "
                        "WHERE trigger_schema='%1' AND trigger_name='oy_st_trg'")
             .arg(db)},
    };
    bool allOk = true;
    for(const Case &cs : cases) {
        /* TRIGGER isn't schema.name-addressable in a DROP (Postgres
         * wants "ON table", not a qualified name) — its own branch
         * below does the right cleanup instead */
        if(cs.kw == QStringLiteral("TRIGGER")) {
            run(QStringLiteral("DROP TRIGGER IF EXISTS \"%1\" ON oy_schema_test_t").arg(cs.name));
            run(QStringLiteral("DROP FUNCTION IF EXISTS \"%1\".\"%2_fn\"").arg(db, cs.name));
        } else {
            run(QStringLiteral("DROP %1 IF EXISTS \"%2\".\"%3\"").arg(cs.kw, db, cs.name));
        }
        QString tmpl = SchemaSql::createTemplate(cs.kw, db, SqlDriverType::Postgres);
        tmpl.replace(QStringLiteral("new_view"), cs.name);
        tmpl.replace(QStringLiteral("new_proc"), cs.name);
        tmpl.replace(QStringLiteral("new_func"), cs.name);
        tmpl.replace(QStringLiteral("new_trigger"), cs.name);
        tmpl.replace(QStringLiteral("some_table"), QStringLiteral("oy_schema_test_t"));
        bool cOk = true;
        for(const QString &s : splitStatements(
                SchemaSql::editorText(cs.kw, db, cs.name, tmpl, true, SqlDriverType::Postgres),
                SqlDriverType::Postgres))
            cOk = run(s) && cOk;
        const bool present = exists(cs.iq) == 1;

        const QString ddl = c->showCreate(cs.kw, db, cs.name, nullptr);
        bool aOk = !ddl.isEmpty();
        for(const QString &s :
            splitStatements(SchemaSql::editorText(cs.kw, db, cs.name, SchemaSql::stripDefiner(ddl),
                                                  false, SqlDriverType::Postgres),
                            SqlDriverType::Postgres))
            aOk = run(s) && aOk;
        const bool stillThere = exists(cs.iq) == 1;

        bool dOk;
        if(cs.kw == QStringLiteral("TRIGGER")) {
            dOk = run(
                QStringLiteral("DROP TRIGGER IF EXISTS \"%1\" ON oy_schema_test_t").arg(cs.name));
            run(QStringLiteral("DROP FUNCTION IF EXISTS \"%1\".\"%2_fn\"")
                    .arg(db, cs.name)); /* the template's companion function */
        } else {
            dOk = run(QStringLiteral("DROP %1 IF EXISTS \"%2\".\"%3\"").arg(cs.kw, db, cs.name));
        }
        const bool gone = exists(cs.iq) == 0;
        const bool caseOk = cOk && present && aOk && stillThere && dOk && gone;
        allOk = allOk && caseOk;
        QTextStream(stdout) << "pgschematest " << cs.kw << ": create=" << cOk
                            << " present=" << present << " alter=" << aOk << " kept=" << stillThere
                            << " drop=" << dOk << " gone=" << gone
                            << (caseOk ? "  PASS\n" : "  FAIL\n");
    }
    run(QStringLiteral("DROP TABLE IF EXISTS oy_schema_test_t"));
    delete c;
    return allOk ? 0 : 1;
}

/* --schematest=host:port:user:pw:db — exercise the schema-object DDL
 * helpers (createTemplate / stripDefiner / alterStatements) against a
 * live server: create → alter → drop a View / Procedure / Function /
 * Trigger / Event, checking information_schema at each step. */
int schemaTest(int &argc, char **argv, const QString &server)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QStringList p = server.split(':');
    if(p.size() != 5) {
        QTextStream(stdout) << "schematest: need host:port:user:pw:db\n";
        return 2;
    }
    ConnectionParams cp;
    cp.host = p[0];
    cp.port = p[1].toUInt();
    cp.user = p[2];
    cp.password = p[3];
    cp.database = p[4];
    QString connectError;
    IDbConnection *c = dbDriverFor(cp.driverType)->connect(cp, &connectError);
    if(!c) {
        QTextStream(stdout) << "schematest: connect failed: " << connectError << '\n';
        return 1;
    }
    const QString db = p[4];
    auto run = [&](const QString &sql) { return c->query(sql, nullptr, nullptr); };
    auto exists = [&](const QString &q) {
        DbResultSet rs;
        if(!c->query(q, &rs, nullptr))
            return -1;
        return rs.rows.isEmpty() ? 0 : rs.rows.first().value(0).toInt();
    };
    struct Case
    {
        QString kw, name, iq;
    };
    const QList<Case> cases = {
        {QStringLiteral("VIEW"), QStringLiteral("oy_st_view"),
         QStringLiteral("SELECT COUNT(*) FROM information_schema.VIEWS "
                        "WHERE TABLE_SCHEMA='%1' AND TABLE_NAME='oy_st_view'")
             .arg(db)},
        {QStringLiteral("PROCEDURE"), QStringLiteral("oy_st_proc"),
         QStringLiteral("SELECT COUNT(*) FROM information_schema.ROUTINES "
                        "WHERE ROUTINE_SCHEMA='%1' AND ROUTINE_NAME='oy_st_proc' "
                        "AND ROUTINE_TYPE='PROCEDURE'")
             .arg(db)},
        {QStringLiteral("FUNCTION"), QStringLiteral("oy_st_func"),
         QStringLiteral("SELECT COUNT(*) FROM information_schema.ROUTINES "
                        "WHERE ROUTINE_SCHEMA='%1' AND ROUTINE_NAME='oy_st_func' "
                        "AND ROUTINE_TYPE='FUNCTION'")
             .arg(db)},
        {QStringLiteral("TRIGGER"), QStringLiteral("oy_st_trg"),
         QStringLiteral("SELECT COUNT(*) FROM information_schema.TRIGGERS "
                        "WHERE TRIGGER_SCHEMA='%1' AND TRIGGER_NAME='oy_st_trg'")
             .arg(db)},
        {QStringLiteral("EVENT"), QStringLiteral("oy_st_event"),
         QStringLiteral("SELECT COUNT(*) FROM information_schema.EVENTS "
                        "WHERE EVENT_SCHEMA='%1' AND EVENT_NAME='oy_st_event'")
             .arg(db)},
    };
    bool allOk = true;
    for(const Case &cs : cases) {
        run(QStringLiteral("DROP %1 IF EXISTS `%2`.`%3`").arg(cs.kw, db, cs.name));
        QString tmpl = SchemaSql::createTemplate(cs.kw, db);
        tmpl.replace(QStringLiteral("new_view"), cs.name);
        tmpl.replace(QStringLiteral("new_proc"), cs.name);
        tmpl.replace(QStringLiteral("new_func"), cs.name);
        tmpl.replace(QStringLiteral("new_trigger"), cs.name);
        tmpl.replace(QStringLiteral("new_event"), cs.name);
        tmpl.replace(QStringLiteral("some_table"), QStringLiteral("employees"));
        bool cOk = true;
        for(const QString &s :
            splitStatements(SchemaSql::editorText(cs.kw, db, cs.name, tmpl, true)))
            cOk = run(s) && cOk;
        const bool present = exists(cs.iq) == 1;
        /* alter round-trip */
        const QString ddl = c->showCreate(cs.kw, db, cs.name, nullptr);
        bool aOk = !ddl.isEmpty();
        for(const QString &s : splitStatements(
                SchemaSql::editorText(cs.kw, db, cs.name, SchemaSql::stripDefiner(ddl), false)))
            aOk = run(s) && aOk;
        const bool stillThere = exists(cs.iq) == 1;
        const bool dOk = run(QStringLiteral("DROP %1 IF EXISTS `%2`.`%3`").arg(cs.kw, db, cs.name));
        const bool gone = exists(cs.iq) == 0;
        const bool caseOk = cOk && present && aOk && stillThere && dOk && gone;
        allOk = allOk && caseOk;
        QTextStream(stdout) << "schematest " << cs.kw << ": create=" << cOk
                            << " present=" << present << " alter=" << aOk << " kept=" << stillThere
                            << " drop=" << dOk << " gone=" << gone
                            << (caseOk ? "  PASS\n" : "  FAIL\n");
    }
    delete c;
    return allOk ? 0 : 1;
}

} // namespace Selftest
