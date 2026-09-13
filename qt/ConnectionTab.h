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
#include "CodeEditor.h"
#include "QueryModel.h"

#include <QComboBox>
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
namespace SqlDump { struct Options; }

/* one statement's outcome, collected on the worker thread */
struct QueryResult
{
    bool     ok      = false;
    double   secs    = 0.0;
    QString  message;
    QStringList      headers;
    QVector<QStringList> rows;
};

class ConnectionTab : public QWidget
{
    Q_OBJECT
public:
    explicit ConnectionTab(const ConnectionParams &params, QWidget *parent = nullptr);
    ~ConnectionTab() override;

    bool isConnected() const { return m_conn != nullptr; }
    QString title() const { return m_params.name; }
    QStringList databases() const { return m_databases; }
    QString currentDatabase() const { return m_params.database; }
    QString hostLabel() const
    {
        return m_params.driverType == DriverType::Sqlite
            ? m_params.filePath
            : m_params.host + ':' + QString::number(m_params.port);
    }
    /* [db, table] of the browser's selected table, or empty */
    QStringList selectedTableInfo() const;
    DriverType driverType() const { return m_params.driverType; }
    const ConnectionParams &params() const { return m_params; }

    /* Query timeout, persisted in OpenYog.ini [Query] (0 = disabled, the
     * default). Global, not per-tab — mirrors Theme::load()/save(). */
    static int  queryTimeoutSecs();
    static void setQueryTimeoutSecs(int secs);

public slots:
    void runQuery();                 /* F9 — selection or current statement */
    void runAll();                   /* Ctrl+F9 — the whole editor */
    void runAndEdit();               /* F8 — run, and open a single-table SELECT editable */
    void explainCurrent(bool json);  /* EXPLAIN [FORMAT=JSON] the current stmt */
    void renameCurrentEditorTab();   /* Alt+F2 */
    void dumpTable(const QString &db, const QString &table);   /* one-table SQL dump */
    void editorCopyNormalizedWhitespace();   /* Alt+C */
    void editorInsertFromFile();
    void collapseBrowser();
    void runStatements(const QStringList &statements, const QString &tabPrefix);
    void cancelQuery();   /* best-effort: ask the in-flight batch to stop */
    void openTable(const QString &db, const QString &table);
    void useDatabase(const QString &db);
    void refreshBrowser();
    bool execDdl(const QString &sql);
    void pasteSqlTemplate(int kind);   /* 0=INSERT 1=UPDATE 2=DELETE 3=SELECT */
    void toggleBrowserPane();
    void toggleResultPane();
    void toggleEditorPane();
    void openSelectedTable();
    void promptCreateTable(const QString &database = {});
    void promptAlterTable(const QString &database, const QString &table);
    void promptRenameTable(const QString &database, const QString &table);
    void promptCopyTable(const QString &database, const QString &table);
    void promptCopyDatabase(const QString &database = {});
    /* non-interactive core for the SQLite branch of promptCopyDatabase,
     * also used by the --sqlitecopydb= selftest */
    bool copySqliteFileTo(const QString &target, bool withData, QString *error);
    void promptImportCsv(const QString &database, const QString &table);
    void promptImportXml(const QString &database, const QString &table);
    /* non-interactive core for the SQLite branch of promptImportCsv (no
     * server-side LOAD DATA on SQLite — parses the file and runs batched
     * INSERT OR IGNORE/REPLACE inside one transaction), also used by the
     * --sqlitecsvimport= selftest */
    bool importCsvIntoSqlite(const QString &db, const QString &table,
                             const QString &file, const QString &sep,
                             const QString &quote, const QString &escCh,
                             bool hasHeader, int extraSkipLines,
                             bool truncateFirst, const QString &onDup,
                             int *rowsInserted, QString *error);
    /* export every row of a table (re-queries — not just the loaded page) */
    void exportTableData(const QString &database, const QString &table);
    /* Tools ▸ Export All Rows… — picks table-data vs result grid by context */
    void exportCurrent();
    void promptUserManager();
    void tableDiagnostics(const QString &database, const QString &table);
    void showConnectionInfo();
    /* non-interactive core, also used by the --copydb selftest */
    bool copyDatabaseTo(const QString &srcDb, const QString &tgtDb,
                        bool withData, bool dropFirst, bool withRoutines,
                        QString *error);
    void promptManageIndexes(const QString &database, const QString &table);
    void promptDropColumn(const QString &database, const QString &table);
    void promptManageForeignKeys(const QString &database, const QString &table);
    void dropTable(const QString &database, const QString &table);
    void truncateTable(const QString &database, const QString &table);
    /* schema objects — objType is VIEW / PROCEDURE / FUNCTION / TRIGGER / EVENT */
    void createSchemaObject(const QString &db, const QString &objType);
    void alterSchemaObject(const QString &db, const QString &objType,
                           const QString &name);
    void dropSchemaObject(const QString &db, const QString &objType,
                          const QString &name);
    void dropDatabase(const QString &db);
    void truncateDatabase(const QString &db);   /* drop every object */
    void emptyDatabase(const QString &db);      /* TRUNCATE every base table */
    void promptAlterDatabase(const QString &db);
    void promptDumpDatabase(const QString &database = {});
    /* non-interactive core, also used by the --dumpdb selftest */
    bool dumpDatabaseToFile(const QString &database, const QString &path,
                            QString *error);
    bool dumpDatabaseToFile(const QString &database, const QString &path,
                            const QStringList &tables, const SqlDump::Options &opt,
                            QString *error);
    void addEditorTab();
    void closeEditorTab(int index);   /* × on a Query / schema-object tab */
    void wireResultGrid(QTableView *grid);   /* right-click menu on a result grid */
    /* new editor tab pre-filled with `sql` and titled `title` (schema-object
     * editors open here, like SQLyog, instead of a modal dialog) */
    CodeEditor *openEditorWithSql(const QString &title, const QString &sql);
    void openTableData(const QString &db, const QString &table);
    void setDataViewMode(const QString &mode);   /* selftest: "text" | "grid" */
    void editTableCell(int row, int col, const QString &value, bool stageOnly = false);
    void openSqlFile(const QString &path);
    void saveEditor();
    void showHistory();
    void renderHistory();
    void clearHistory();
    void exportResult();   /* CSV / HTML / JSON / Markdown, by chosen filter */

    /* Favorites: named, saved SQL snippets (Favorites menu) */
    void addCurrentToFavorites();     /* selection, or whole editor if none */
    void organizeFavorites();         /* rename / delete / insert */
    void insertFavorite(const QString &name);   /* menu item -> editor */

    /* editor Edit-menu ops on the active Query tab */
    void promptFind();
    void findNext();
    void promptReplace();
    void promptGoto();
    void commentSelection(bool add);
    void listTags();               /* force the autocomplete popup */
    void formatQuery(int scope);   /* 0 = current stmt, 1 = selection, 2 = all */

signals:
    void databasesChanged(const QStringList &dbs, const QString &current);
    void executed(const QString &info);      /* "Exec: 0.01 sec" etc. */
    void cursorMoved(const QString &posText);/* "Ln 1, Col 1"        */

private:
    void applyResults(const QVector<QueryResult> &results, const QString &tabPrefix);
    void logHistory(const QString &sql);
    void addResultGrid(const QueryResult &r, const QString &title);

    ConnectionParams   m_params;
    IDbConnection    * m_conn       = nullptr;   /* browsing (GUI thread) */

    ObjectBrowser    * m_browser    = nullptr;
    TableDataView    * m_tableData  = nullptr;
    QLabel           * m_infoBar    = nullptr;
    QComboBox        * m_limitCombo = nullptr;
    QTabWidget       * m_editorTabs = nullptr;
    CodeEditor       * m_editor     = nullptr;
    QWidget          * m_historyPage = nullptr;
    class QTextBrowser * m_history  = nullptr;
    class QLineEdit  * m_historySearch = nullptr;
    QStringList        m_historyLines;    /* display: "[ts] flattened sql" (or divider) */
    QStringList        m_historyQueries;  /* index-aligned: the real query text ("" = divider) */
    void sendHistoryToEditor(const QString &sql);
    void copyAllShownHistory();
    FindBar          * m_findBar    = nullptr;

    QTabWidget       * m_resultTabs = nullptr;
    QPlainTextEdit   * m_messages   = nullptr;
    QLabel           * m_info       = nullptr;
    QTableView       * m_lastGrid   = nullptr;

    QVector<QWidget*>   m_dynamicResultTabs;     /* cleared on each batch */
    double              m_totalSecs = 0.0;
    bool                m_running   = false;
    /* shared (not owned outright): a detached worker thread launched by
     * runStatements() keeps its own reference for the query's duration, so
     * closing this tab mid-query can't leave it pointing at freed memory —
     * see cancelQuery() and runOnConnection() in the .cpp. */
    std::shared_ptr<LiveConnection> m_cancelState;
    int                 m_batchGen  = 0;   /* invalidates a stale timeout timer */
    class QTimer       *m_keepAliveTimer = nullptr;   /* Connect dialog's keep-alive */

    QStringList         m_databases;
    QString             m_lastFind;       /* for Find Next / F3 */
    QStringList         m_completions;    /* schema identifiers for autocomplete (union) */
    QStringList         m_tableNames;     /* offered after FROM / JOIN / INTO / UPDATE */
    QStringList         m_columnNames;    /* offered after SELECT / WHERE / ON / SET … */
    void updateCompletions();

    /* selected table in the browser: [db, table] or empty */
    QStringList currentTableInfo() const;
    CodeEditor  *currentEditor() const;
    void attachEditor(CodeEditor *ed, const QString &title);
};
