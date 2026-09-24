/* OpenYog — one server connection = one tab, laid out like SQLyog:
 *   ┌───────────────┬──────────────────────────────────┐
 *   │ Object Browser│ [Query 1][History][+]  (editor)  │
 *   │ (filter+tree) ├──────────────────────────────────┤
 *   │               │ [1_Messages][Result][Info]       │
 *   └───────────────┴──────────────────────────────────┘
 * Queries execute on a worker thread with a dedicated connection, so the
 * UI never freezes; results are marshalled back to the GUI thread.
 * Mirrors upstream FrameWindow/DataView structure (Phase 3 in plan.md). */
#pragma once

#include "ConnectionParams.h"
#include "SqlEditor.h"
#include "QueryModel.h"

#include <QComboBox>
#include <QHash>
#include <QLabel>
#include <QPointer>
#include <QTableView>
#include <QTabWidget>
#include <QWidget>

#include <QStringList>
#include <QVector>

#include <memory>

class IDbConnection;
class LiveConnection;
class ObjectBrowser;
class TableDataView;
class FindBar;
class QPlainTextEdit;
namespace SqlDump {
struct Options;
}

/* one statement's outcome, collected on the worker thread */
struct QueryResult
{
    bool ok = false;
    double secs = 0.0;
    QString message;
    QStringList headers;
    QVector<QStringList> rows;
};

class ConnectionTab : public QWidget
{
    Q_OBJECT
public:
    explicit ConnectionTab(const ConnectionParams &params, QWidget *parent = nullptr);
    ~ConnectionTab() override;

    bool isConnected() const
    {
        return m_conn != nullptr;
    }
    QString title() const
    {
        return m_params.name;
    }
    QStringList databases() const
    {
        return m_databases;
    }
    QString currentDatabase() const
    {
        return m_params.database;
    }
    /* the schema/database actually being *browsed* right now — same as
     * currentDatabase() for MySQL/SQLite, but for PostgreSQL that's the
     * connected database (needed for the libpq conninfo on reconnect),
     * not a schema; this is the toolbar-combo-selected schema instead
     * (see the member doc comment below). MainWindow's toolbar sync uses
     * this, not currentDatabase(), so the combo shows the right thing
     * for Postgres when there's more than one schema. */
    QString defaultDb() const;
    /* has the user actually picked a schema for this tab yet (via the
     * toolbar combo), as opposed to defaultDb() just falling back to
     * "public" because m_currentSchema is still empty? Always true for
     * MySQL/SQLite (currentDatabase() there was an explicit choice
     * already, made in the Connect dialog, not an assumed default).
     * MainWindow's toolbar combo uses this to decide whether to show
     * defaultDb() or stay blank/unselected — pre-filling "public" as
     * though it were chosen, when it's really just this class's own
     * fallback, reads as the app deciding for the user rather than
     * reflecting a real choice. */
    bool hasExplicitSchema() const
    {
        return m_params.driverType != SqlDriverType::Postgres || !m_currentSchema.isEmpty();
    }
    /* PostgreSQL only: an already-open connection to `database` on this
     * same server, opening and caching one on first request (see
     * m_sideConnections' doc comment). `database` empty or equal to this
     * tab's own connected database just returns m_conn. Returns nullptr
     * (with *error set) if opening a new connection fails — a permissions
     * issue or a database dropped out from under the browser, not
     * something to crash over. Used by ObjectBrowser's multi-database view. */
    IDbConnection *connectionFor(const QString &database, QString *error);
    /* ConnectionParams for a fresh connection to `database` on this same
     * server (same host/port/user/password/driver as this tab) — used both
     * by connectionFor() above and by "open this database in its own tab"
     * from the Object Browser's multi-database view. */
    ConnectionParams paramsFor(const QString &database) const;
    /* PostgreSQL only: makes `database` this tab's own primary connection
     * in place — opens (or reuses a cached) connection to it via
     * connectionFor(), swaps it in as m_conn, and stashes the outgoing
     * primary as a side connection under its own name (so switching back
     * is instant, not a reconnect). Rebuilds the browser tree, resets the
     * schema tracking (m_currentSchema) since it belonged to the old
     * database, and updates the tab's own name/title. Returns false (with
     * a message left in the Messages pane) if the new connection fails.
     * This is what double-clicking a database node (or its "Switch to
     * `db`" menu entry) does in the Object Browser's tree — the default
     * there, since staying in one tab is normally what's wanted;
     * openDatabaseInNewTabRequested's "Connect in New Tab" is the explicit
     * alternative for comparing two databases side by side. The toolbar
     * combo itself only ever lists the current database's schemas (see
     * MainWindow::syncToolbarToCurrentTab()) — switching databases is the
     * tree's job, not the combo's. */
    bool switchDatabase(const QString &database);
    QString hostLabel() const
    {
        return m_params.driverType == SqlDriverType::Sqlite
                   ? m_params.filePath
                   : m_params.host + ':' + QString::number(m_params.port);
    }
    /* what the status bar's connection field should show right now: the
     * tab's primary connection in "user@host:port/database" form (SQLite:
     * the file path) — except while the visible Table Data grid was loaded
     * through a PostgreSQL side connection to another physical database,
     * when it names THAT connection with a "— table data" marker, so the
     * footer never claims the primary is showing another database's rows
     * (the whole reason this exists: after the foreign-table fix, the
     * title bar correctly stays on the primary while the grid doesn't) */
    QString activeConnectionLabel() const;
    /* how many server connections this tab actually holds open right now
     * — the primary plus every PostgreSQL side connection still in the
     * pool. The status bar's "Connections: N" counts these, not tabs;
     * the two differ the moment the multi-database tree is used. */
    int liveConnectionCount() const
    {
        return m_conn ? 1 + m_sideConnections.size() : 0;
    }
    /* [db, table] of the browser's selected table, or empty */
    QStringList selectedTableInfo() const;
    SqlDriverType driverType() const
    {
        return m_params.driverType;
    }
    /* status-bar name: the MySQL driver serves both, so say which server this is */
    QString driverDisplayLabel() const;
    const ConnectionParams &params() const
    {
        return m_params;
    }

    /* Query timeout, persisted in OpenYog.ini [Query] (0 = disabled, the
     * default). Global, not per-tab — mirrors Theme::load()/save(). */
    static int queryTimeoutSecs();
    static void setQueryTimeoutSecs(int secs);

public slots:
    void runQuery();                /* F9 — selection or current statement */
    void runAll();                  /* Ctrl+F9 — the whole editor */
    void runAndEdit();              /* F8 — run, and open a single-table SELECT editable */
    void explainCurrent(bool json); /* EXPLAIN [FORMAT=JSON] the current stmt */
    void selftestExplain(const QString &mode); /* --explain=json|plain selftest */
    QString selftestShowDataTab(); /* --showdatatab selftest: switch to Table Data; its table */
    void selftestShowInfoTab();    /* --showinfotab selftest: switch to the Info result tab */
    void renameCurrentEditorTab(); /* Alt+F2 */
    void dumpTable(const QString &db, const QString &table); /* one-table SQL dump */
    void editorCopyNormalizedWhitespace();                   /* Alt+C */
    void editorInsertFromFile();
    void collapseBrowser();
    void runStatements(const QStringList &statements, const QString &tabPrefix);
    void cancelQuery(); /* best-effort: ask the in-flight batch to stop */
    void openTable(const QString &db, const QString &table);
    void useDatabase(const QString &db);
    /* autoDrill forwarded to ObjectBrowser::loadDatabases() — false only
     * from switchDatabase(), so promoting a tree database to primary just
     * reveals its schema list instead of also diving into its Tables */
    void refreshBrowser(bool autoDrill = true);
    /* headless-test hook, forwards to ObjectBrowser::expandTopLevelDatabase() */
    void expandDatabaseNode(const QString &name);
    /* headless-test hook, forwards to ObjectBrowser::selectTreeItem() */
    void selectBrowserItem(const QString &path);
    /* headless-test hook, forwards to ObjectBrowser::clickTreeItem() */
    void clickBrowserItem(const QString &path);
    /* headless-test hook, forwards to ObjectBrowser::collapseTreeItem() */
    void collapseBrowserItem(const QString &path);
    /* headless-test hook, forwards to ObjectBrowser::expandTreeItem() */
    void expandBrowserItem(const QString &path);
    QStringList dumpBrowserSubtree(const QString &path);      /* --dumptree= selftest */
    QStringList dumpEditorText();                             /* --dumpeditor selftest */
    QStringList browserContextMenuItems(const QString &path); /* --treemenu= selftest */
    /* headless-test hook, forwards to ObjectBrowser::doubleClickTreeItem() */
    void doubleClickBrowserItem(const QString &path);
    bool execDdl(const QString &sql);
    void pasteSqlTemplate(int kind); /* 0=INSERT 1=UPDATE 2=DELETE 3=SELECT */
    void toggleBrowserPane();
    void toggleResultPane();
    void toggleEditorPane();
    void openSelectedTable();
    void promptCreateTable(const QString &database = {});
    void promptAlterTable(const QString &database, const QString &table);
    void promptRenameTable(const QString &database, const QString &table);
    void promptCopyTable(const QString &database, const QString &table);
    void promptCopyTableToHost(const QString &database, const QString &table);
    void promptCopyDatabase(const QString &database = {});
    /* non-interactive core for the SQLite branch of promptCopyDatabase,
     * also used by the --sqlitecopydb= selftest */
    bool copySqliteFileTo(const QString &target, bool withData, QString *error);
    void promptImportCsv(const QString &database, const QString &table);
    void promptImportXml(const QString &database, const QString &table);
    /* non-interactive core for the SQLite/PostgreSQL branch of
     * promptImportCsv (neither has MySQL's LOAD DATA LOCAL INFILE — parses
     * the file itself and runs batched INSERT inside one transaction: SQLite
     * via INSERT OR IGNORE/REPLACE, PostgreSQL via INSERT ... ON CONFLICT),
     * also used by the --sqlitecsvimport=/--pgcsvimport= selftests */
    bool importCsvBatched(const QString &db, const QString &table, const QString &file,
                          const QString &sep, const QString &quote, const QString &escCh,
                          bool hasHeader, int extraSkipLines, bool truncateFirst,
                          const QString &onDup, int *rowsInserted, QString *error);
    /* export every row of a table (re-queries — not just the loaded page) */
    void exportTableData(const QString &database, const QString &table);
    /* Tools ▸ Export All Rows… — picks table-data vs result grid by context */
    void exportCurrent();
    void promptUserManager();
    void tableDiagnostics(const QString &database, const QString &table);
    void showTableProperties(const QString &database, const QString &table);
    void showConnectionInfo();
    /* non-interactive core, also used by the --copydb selftest */
    bool copyDatabaseTo(const QString &srcDb, const QString &tgtDb, bool withData, bool dropFirst,
                        bool withRoutines, QString *error);
    /* PostgreSQL's same-connection analog of copyDatabaseTo() — "database"
     * here means schema (a Postgres connection can't reach a sibling
     * database at all), so this is schema-to-schema within the same
     * connection, also used by the --pgcopydb selftest */
    bool copyDatabaseToPostgres(const QString &srcSchema, const QString &tgtSchema, bool withData,
                                bool dropFirst, bool withRoutines, QString *error);
    void promptManageIndexes(const QString &database, const QString &table);
    void promptDropColumn(const QString &database, const QString &table);
    void promptManageForeignKeys(const QString &database, const QString &table);
    void dropTable(const QString &database, const QString &table);
    void truncateTable(const QString &database, const QString &table);
    /* schema objects — objType is VIEW / PROCEDURE / FUNCTION / TRIGGER / EVENT */
    void createSchemaObject(const QString &db, const QString &objType);
    void alterSchemaObject(const QString &db, const QString &objType, const QString &name);
    void dropSchemaObject(const QString &db, const QString &objType, const QString &name);
    void dropDatabase(const QString &db);
    void truncateDatabase(const QString &db); /* drop every object */
    void emptyDatabase(const QString &db);    /* TRUNCATE every base table */
    void promptAlterDatabase(const QString &db);
    void promptDumpDatabase(const QString &database = {});
    void promptSchemaHtml(const QString &database = {});
    void promptDataSearch(const QString &database = {});
    /* non-interactive core, also used by the --schemahtmltest= selftest */
    QString buildSchemaHtml(const QString &db);
    /* non-interactive core, also used by the --dumpdb selftest */
    bool dumpDatabaseToFile(const QString &database, const QString &path, QString *error);
    bool dumpDatabaseToFile(const QString &database, const QString &path, const QStringList &tables,
                            const SqlDump::Options &opt, QString *error);
    void addEditorTab();
    void closeEditorTab(int index);        /* × on a Query / schema-object tab */
    void closeResultTab(int index);        /* × on an "Execute Query N" result tab */
    void wireResultGrid(QTableView *grid); /* right-click menu on a result grid */
    /* new editor tab pre-filled with `sql` and titled `title` (schema-object
     * editors open here, like SQLyog, instead of a modal dialog) */
    SqlEditor *openEditorWithSql(const QString &title, const QString &sql);
    void openTableData(const QString &db, const QString &table);
    void setDataViewMode(const QString &mode);       /* selftest: "text" | "grid" */
    QString cellTextForTest(int row, int col) const; /* selftest: grid cell's displayed text */
    void editTableCell(int row, int col, const QString &value, bool stageOnly = false);
    void openSqlFile(const QString &path);
    void saveEditor();
    void showHistory();
    void renderHistory();
    void clearHistory();
    void exportResult(); /* CSV / HTML / JSON / Markdown, by chosen filter */

    /* Favorites: named, saved SQL snippets (Favorites menu) */
    void addCurrentToFavorites();             /* selection, or whole editor if none */
    void organizeFavorites();                 /* rename / delete / insert */
    void insertFavorite(const QString &name); /* menu item -> editor */

    /* editor Edit-menu ops on the active Query tab */
    void promptFind();
    void findNext();
    void promptReplace();
    void promptGoto();
    void commentSelection(bool add);
    void listTags();             /* force the autocomplete popup */
    void formatQuery(int scope); /* 0 = current stmt, 1 = selection, 2 = all */

signals:
    void databasesChanged(const QStringList &dbs, const QString &current);
    void executed(const QString &info);       /* "Exec: 0.01 sec" etc. */
    void cursorMoved(const QString &posText); /* "Ln 1, Col 1"        */
    /* PostgreSQL multi-database tree only: user asked to open a different
     * physical database as its own tab (see ObjectBrowser::
     * openDatabaseInNewTabRequested()'s doc comment) — MainWindow actually
     * owns the tab widget, so this bubbles the request up to it. */
    void newTabRequested(const ConnectionParams &params);
    /* the connection serving what's on screen changed (Table Data loaded
     * through a side connection, a result tab switched away from it, the
     * primary was swapped by switchDatabase, a side connection opened) —
     * carries activeConnectionLabel() for the status bar, which
     * MainWindow also uses as the cue to re-count live connections */
    void activeConnectionChanged(const QString &label);

private:
    void applyResults(const QVector<QueryResult> &results, const QString &tabPrefix);
    void logHistory(const QString &sql);
    void addResultGrid(const QueryResult &r, const QString &title);
    /* Object Browser single-click → the Info tab (upstream ObjectInfo.cpp):
     * Column/Index/Foreign-Key info + DDL for a table/view, DDL alone for a
     * procedure/function/trigger/event. physDb routes to the right side
     * connection (Postgres multi-database tree), empty on MySQL/SQLite. */
    void updateInfoTab(const QString &db, const QString &objType, const QString &name,
                       const QString &physDb);
    QString buildObjectInfoHtml(IDbConnection *conn, const QString &db, const QString &objType,
                                const QString &name);

    ConnectionParams m_params;
    IDbConnection *m_conn = nullptr; /* browsing (GUI thread) */
    /* PostgreSQL only: side connections to *other* physical databases on
     * the same server, opened lazily the first time the Object Browser's
     * multi-database view expands one, keyed by database name. A Postgres
     * connection can't query another database at all (see
     * PostgresConnection.h), so seeing "every database" from one tab means
     * quietly holding one live connection per database the user has
     * actually looked at — reused after the first open, closed with the
     * rest of the tab in the destructor. */
    QHash<QString, IDbConnection *> m_sideConnections;

    ObjectBrowser *m_browser = nullptr;
    TableDataView *m_tableData = nullptr;
    QTabWidget *m_editorTabs = nullptr;
    SqlEditor *m_editor = nullptr;
    QWidget *m_historyPage = nullptr;
    class QTextBrowser *m_history = nullptr;
    class QLineEdit *m_historySearch = nullptr;
    QStringList m_historyLines;   /* display: "[ts] flattened sql" (or divider) */
    QStringList m_historyQueries; /* index-aligned: the real query text ("" = divider) */
    void sendHistoryToEditor(const QString &sql);
    void copyAllShownHistory();
    FindBar *m_findBar = nullptr;

    QTabWidget *m_resultTabs = nullptr;
    QPlainTextEdit *m_messages = nullptr;
    /* upstream ObjectInfo.cpp: Column/Index/Foreign-Key/DDL metadata for
     * whatever schema object was last single-clicked in the tree — an
     * HTML table + <pre> DDL block, not query output, hence QTextBrowser
     * (same widget m_history already uses) rather than a plain QLabel */
    class QTextBrowser *m_info = nullptr;
    QTableView *m_lastGrid = nullptr;

    QVector<QWidget *>
        m_dynamicResultTabs;    /* one per query run, closable; stay until the user closes them */
    int m_resultTabCounter = 0; /* ever-increasing, not reset per run — keeps titles unique */
    double m_totalSecs = 0.0;
    bool m_running = false;
    /* shared (not owned outright): a detached worker thread launched by
     * runStatements() keeps its own reference for the query's duration, so
     * closing this tab mid-query can't leave it pointing at freed memory —
     * see cancelQuery() and runOnConnection() in the .cpp. */
    std::shared_ptr<LiveConnection> m_cancelState;
    int m_batchGen = 0;                       /* invalidates a stale timeout timer */
    class QTimer *m_keepAliveTimer = nullptr; /* Connect dialog's keep-alive */

    QStringList m_databases;
    QString m_lastFind;        /* for Find Next / F3 */
    QStringList m_completions; /* schema identifiers for autocomplete (union) */
    QStringList m_tableNames;  /* offered after FROM / JOIN / INTO / UPDATE */
    QStringList m_columnNames; /* offered after SELECT / WHERE / ON / SET … */
    struct PendingTable
    {
        QString db, table, physDb;
    };
    PendingTable m_pendingTable;               /* last table clicked, not yet in the grid */
    QString m_loadedTableKey;                  /* physDb, db, table of the grid's content */
    QHash<QString, QStringList> m_columnCache; /* completion: db + table → columns */
    void updateCompletions();
    void loadTableData(const QString &db, const QString &table, const QString &physDb,
                       bool activate);
    void showPendingTable();
    void selectSoleSchema(); /* Postgres: one schema → current, no click needed */
    /* defaultDb() (see public section above): the schema/database to
     * operate on when nothing more specific was selected (no table chosen
     * in the browser, no explicit db argument) — for MySQL/SQLite this is
     * exactly m_params.database (the database USE'd or attached at connect
     * time); for PostgreSQL it falls back to m_currentSchema instead,
     * since m_params.database there is the *connected* database (needed
     * for the libpq conninfo, see PostgresConnection.h), not a schema.
     * PostgreSQL only: the schema useDatabase()'s SET search_path last
     * switched to — kept separate from m_params.database on purpose, since
     * that field must go on meaning "the connected database" for
     * reconnects/Copy Connection/Session save, not "the schema currently
     * being browsed". Empty until useDatabase() is called at least once
     * (defaultDb() falls back to "public" for that case). */
    QString m_currentSchema;

    /* which physical database's connection loaded the Table Data grid's
     * current rows — empty when it was (or last was) this tab's own
     * primary connection, the database name when it went through a
     * PostgreSQL side connection. activeConnectionLabel() shows the side
     * connection only while that grid is the visible result tab; any
     * switch away from it (Messages, a query's result grid) drops back
     * to the primary's identity. Cleared by switchDatabase(): the tree
     * was rebuilt, so the grid's rows are from a connection that may
     * since have moved back into the pool. */
    QString m_tableDataPhysDb;
    void updateActiveConnectionLabel();

    /* selected table in the browser: [db, table] or empty */
    QStringList currentTableInfo() const;
    SqlEditor *currentEditor() const;
    void attachEditor(SqlEditor *ed, const QString &title);
};
