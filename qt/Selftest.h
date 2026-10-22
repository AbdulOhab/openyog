/* OpenYog — the standalone selftests: flags qt/main.cpp hands straight off to
 * instead of starting the app. Each sets up only what it needs (its own
 * QApplication when it draws or connects), prints a PASS/FAIL line per check
 * and returns the process exit code, 0 when every check passed. The selftest
 * flags that drive a running MainWindow stay in main.cpp. */
#pragma once

#include <QList>
#include <QPair>
#include <QString>

class IDbConnection;

namespace Selftest {

/* SelftestSqlite.cpp, each against a prepared SQLite file:
 * --sqlitetest=, --sqlitecreatetabletest=, --sqliteindextest=,
 * --sqlitemisctest= */
int sqliteTest(int &argc, char **argv, const QString &file);
int sqliteCreateTableTest(int &argc, char **argv, const QString &path);
int sqliteIndexTest(int &argc, char **argv, const QString &path);
int sqliteMiscTest(int &argc, char **argv, const QString &path);

/* SelftestServer.cpp, server is host:port:user:pw:db:
 * --pgtest=, --pgschematest=, --schematest= (MySQL) */
int pgTest(int &argc, char **argv, const QString &server);
int pgSchemaTest(int &argc, char **argv, const QString &server);
int schemaTest(int &argc, char **argv, const QString &server);

/* SelftestMisc.cpp: --exporttest=DIR, --shortcuttest, --stmtattest,
 * --casetest, --icontest, --filtertest, --keychaintest, --comptest */
int exportTest(const QString &dir);
int shortcutTest(int &argc, char **argv);
int stmtAtTest();
int caseTest(int &argc, char **argv);
int iconTest(int &argc, char **argv);
int filterTest();
int keychainTest(int &argc, char **argv);
int compTest(int &argc, char **argv);

/* the driver selftests' shared part (SelftestServer.cpp): queryLimited()
 * keeps and counts what it should and leaves the connection usable; one
 * (what, passed) per check */
QList<QPair<QString, bool>> queryLimitedChecks(IDbConnection *c);

} // namespace Selftest
