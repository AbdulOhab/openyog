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

class ObjectBrowser;
class TableDataView;
class FindBar;
class QPlainTextEdit;

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
        return m_params.host + ':' + QString::number(m_params.port);
    }
    /* [db, table] of the browser's selected table, or empty */
    QStringList selectedTableInfo() const;

public slots:
    void runQuery();
    void runStatements(const QStringList &statements, const QString &tabPrefix);
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
    void promptImportCsv(const QString &database, const QString &table);
    void promptUserManager();
    /* non-interactive core, also used by the --copydb selftest */
    bool copyDatabaseTo(const QString &srcDb, const QString &tgtDb,
                        bool withData, bool dropFirst, bool withRoutines,
                        QString *error);
    void promptManageIndexes(const QString &database, const QString &table);
    void promptManageForeignKeys(const QString &database, const QString &table);
    void dropTable(const QString &database, const QString &table);
    void truncateTable(const QString &database, const QString &table);
    void promptDumpDatabase(const QString &database = {});
    /* non-interactive core, also used by the --dumpdb selftest */
    bool dumpDatabaseToFile(const QString &database, const QString &path,
                            QString *error);
    void addEditorTab();
    void openTableData(const QString &db, const QString &table);
    void editTableCell(int row, int col, const QString &value, bool stageOnly = false);
    void openSqlFile(const QString &path);
    void saveEditor();
    void showHistory();
    void exportResult();   /* CSV / HTML / JSON / Markdown, by chosen filter */

    /* editor Edit-menu ops on the active Query tab */
    void promptFind();
    void findNext();
    void promptReplace();
    void promptGoto();
    void commentSelection(bool add);

signals:
    void databasesChanged(const QStringList &dbs, const QString &current);
    void executed(const QString &info);      /* "Exec: 0.01 sec" etc. */
    void cursorMoved(const QString &posText);/* "Ln 1, Col 1"        */

private:
    void applyResults(const QVector<QueryResult> &results, const QString &tabPrefix);
    void logHistory(const QString &sql);
    void addResultGrid(const QueryResult &r, const QString &title);

    ConnectionParams   m_params;
    MYSQL            * m_conn       = nullptr;   /* browsing (GUI thread) */

    ObjectBrowser    * m_browser    = nullptr;
    TableDataView    * m_tableData  = nullptr;
    QLabel           * m_infoBar    = nullptr;
    QComboBox        * m_limitCombo = nullptr;
    QTabWidget       * m_editorTabs = nullptr;
    CodeEditor       * m_editor     = nullptr;
    QPlainTextEdit   * m_history    = nullptr;
    FindBar          * m_findBar    = nullptr;

    QTabWidget       * m_resultTabs = nullptr;
    QPlainTextEdit   * m_messages   = nullptr;
    QLabel           * m_info       = nullptr;
    QTableView       * m_lastGrid   = nullptr;

    QVector<QWidget*>   m_dynamicResultTabs;     /* cleared on each batch */
    double              m_totalSecs = 0.0;
    bool                m_running   = false;

    QStringList         m_databases;
    QString             m_lastFind;       /* for Find Next / F3 */

    /* selected table in the browser: [db, table] or empty */
    QStringList currentTableInfo() const;
    CodeEditor  *currentEditor() const;
    void attachEditor(CodeEditor *ed, const QString &title);
};
