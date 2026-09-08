/* port/smoketest.cpp — Phase 1 verification (plan.md)
 *
 * Proves two things on native Linux:
 *   1. wyString — the string class the entire SQLyog core is built on —
 *      compiles and behaves (formatting, case-insensitive compare, wide-char
 *      round-trip, chunked allocation, base64 helpers from port/stubs.cpp).
 *   2. The bundled MariaDB client path connects and runs queries (session-
 *      local TEMPORARY table only, zero footprint on the server).
 *
 * Usage: smoketest [host] [user] [password] [port] [db]
 * Exit codes: 0 all pass incl. DB | 1 unit failures | 3 unit ok, DB unreachable
 */

#include "wyString.h"
#include "CommonHelper.h"   /* port shim: _(), base64 + log stubs */
#include "wyFile.h"
#include "wyIni.h"

#include <mysql/mysql.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static int g_failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if(!ok)
        g_failures++;
}

static void check_sql(bool ok, MYSQL *c, const char *what)
{
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                ok ? "" : " -> ", ok ? "" : mysql_error(c));
    if(!ok)
        g_failures++;
}

static void test_wystring()
{
    std::printf("wyString unit tests:\n");

    wyString s;
    s.SetAs("SELECT ");
    s.Add("1 + 1");
    check(s.GetLength() == 12, "SetAs + Add + GetLength");

    wyString q;
    q.Sprintf("%s AS answer", s.GetString());
    check(q.Compare("SELECT 1 + 1 AS answer") == 0, "Sprintf from another wyString");
    check(q.CompareI("select 1 + 1 AS ANSWER") == 0, "CompareI (case-insensitive)");

    wyString w;
    w.SetAs(L"wchar round-trip ✓");
    check(strcmp(w.GetString(), "wchar round-trip \xe2\x9c\x93") == 0,
          "wide-char (wyWChar) in / UTF-8 out");

    wyString big;
    for(int i = 0; i < 40; i++)
        big.Add("0123456789");
    check(big.GetLength() == 400, "chunked growth past BUF_CHUNK_ALLOCATION (256)");

    wyChar *b64 = NULL;
    EncodeBase64("hello", 5, &b64);
    check(b64 && strcmp(b64, "aGVsbG8=") == 0, "EncodeBase64");

    wyChar decoded[16] = {};
    size_t rawlen = DecodeBase64(b64, decoded);
    check(rawlen == 5 && strcmp(decoded, "hello") == 0, "DecodeBase64 round-trip");
    free(b64);
}

static void test_wyfile()
{
    std::printf("wyFile unit tests:\n");

    wyString dir, path;
    wyFile f;
    f.GetTempFilePath(&dir);
    path.SetAs(dir.GetString());
    path.Add("/port_smoke_test.tmp");
    f.SetFilename(&path);

    check(f.CheckIfFileExists() == wyFalse, "GetTempFilePath + file absent");
    check(f.OpenWithPermission(GENERIC_WRITE, CREATE_NEW) != -1,
          "OpenWithPermission(GENERIC_WRITE, CREATE_NEW)");

    const char *msg = "linux port works";
    bool wrote = write(f.m_hfile, msg, strlen(msg)) == (ssize_t)strlen(msg);
    check(wrote, "write() through wyFile fd");
    check(f.Close() == 0, "Close()");

    check(f.CheckIfFileExists() == wyTrue, "CheckIfFileExists after write");
    check(f.RemoveFile() == 1, "RemoveFile");
    check(f.CheckIfFileExists() == wyFalse, "file gone after RemoveFile");
}

static void test_wyini()
{
    std::printf("wyIni unit tests (INI round-trip = SQLyog connection storage path):\n");

    wyString dir, path;
    wyFile f;
    f.GetTempFilePath(&dir);
    path.SetAs(dir.GetString());
    path.Add("/port_smoke_test.ini");

    check(wyIni::IniWriteString("port_smoke", "engine", "mariadb", path.GetString()) == wyTrue,
          "IniWriteString");
    check(wyIni::IniWriteInt("port_smoke", "answer", 42, path.GetString()) == wyTrue,
          "IniWriteInt");

    wyString got;
    wyIni::IniGetString("port_smoke", "engine", "", &got, path.GetString());
    check(got.Compare("mariadb") == 0, "IniGetString round-trip");
    check(wyIni::IniGetInt("port_smoke", "answer", 0, path.GetString()) == 42,
          "IniGetInt round-trip");
    check(wyIni::IniGetString("port_smoke", "missing", "fallback", &got, path.GetString()) != 0
          && got.Compare("fallback") == 0, "IniGetString default on missing key");

    wyFile cleaner;
    cleaner.SetFilename(&path);
    cleaner.RemoveFile();
}

static void test_mariadb(const char *host, const char *user, const char *pass, int port, const char *db)
{
    std::printf("\nMariaDB connectivity (host=%s port=%d user=%s db=%s):\n", host, port, user, db);

    if(mysql_library_init(0, NULL, NULL)) {
        std::printf("  [FAIL] mysql_library_init\n");
        g_failures++;
        return;
    }

    MYSQL *c = mysql_init(NULL);
    if(!c) {
        std::printf("  [FAIL] mysql_init\n");
        g_failures++;
        return;
    }
    mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if(!mysql_real_connect(c, host, user, pass, db, port, NULL, 0)) {
        std::printf("  [SKIP] cannot connect: %s\n", mysql_error(c));
        std::printf("  (is the local server running? see WORKLOG for setup)\n");
        mysql_close(c);
        mysql_library_end();
        exit(3);
    }
    std::printf("  [PASS] mysql_real_connect\n");

    wyString q, note;
    MYSQL_ROW row;

    q.Sprintf("SELECT VERSION(), CURRENT_USER()");
    bool ok = mysql_query(c, q.GetString()) == 0;
    check(ok, q.GetString());
    MYSQL_RES *res = ok ? mysql_store_result(c) : NULL;   /* always fresh */
    if(res && (row = mysql_fetch_row(res)))
        std::printf("         server=%s as %s\n", row[0], row[1]);
    if(res) mysql_free_result(res);

    ok = mysql_query(c, "CREATE TEMPORARY TABLE port_smoke (id INT PRIMARY KEY, note VARCHAR(100)) "
                        "CHARACTER SET utf8mb4") == 0;
    check_sql(ok, c, "CREATE TEMPORARY TABLE port_smoke");

    q.Sprintf("INSERT INTO port_smoke VALUES (1, 'linux port works')");
    ok = mysql_query(c, q.GetString()) == 0;
    check_sql(ok, c, q.GetString());

    ok = mysql_query(c, "SELECT note FROM port_smoke WHERE id = 1") == 0;
    res = ok ? mysql_store_result(c) : NULL;
    if(res && (row = mysql_fetch_row(res)))
        note.SetAs(row[0]);
    check(note.Compare("linux port works") == 0, "SELECT via wyString-built query");
    if(res) mysql_free_result(res);

    mysql_close(c);
    mysql_library_end();
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *user = argc > 2 ? argv[2] : "root";
    const char *pass = argc > 3 ? argv[3] : "";
    int         port = argc > 4 ? atoi(argv[4]) : 3306;
    const char *db   = argc > 5 ? argv[5] : "port_test";

    test_wystring();
    test_wyfile();
    test_wyini();
    test_mariadb(host, user, pass, port, db);

    std::printf("\n%s (%d failure%s)\n",
                g_failures ? "SMOKE TEST FAILED" : "SMOKE TEST PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
