/* OpenYog — main window.
 * The menus are transcribed from the upstream resource script
 * (include/SQLyog.rc, IDR_MAINMENU): labels, nesting and shortcuts are
 * verbatim. Items whose functionality hasn't been ported yet are visible but
 * disabled — they mark the roadmap (FEATURES.md), they are not dead weight. */
#pragma once

#include <QMainWindow>

#include "ConnectionParams.h"

class QComboBox;
class QLabel;
class QTabWidget;
class ConnectionTab;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void newConnection();
    void closeTab(int index);
    void syncToolbarToCurrentTab();
    void useDatabaseFromCombo(const QString &db);
    void executeCurrentTab();
    void refreshBrowser();
    void createDatabase();
    void createTable(const QString &database = {});
    void alterTable();
    void dumpDatabase(const QString &database = {});
    void editClipboard(const QString &what);
    void switchTab(int delta);

public:
    /* programmatic path used by the --autoconnect selftest (main.cpp) */
    bool openAndRun(const ConnectionParams &params);
    void openTableData(const QString &db, const QString &table);
    void setDataViewMode(const QString &mode);   /* selftest: "text" | "grid" */
    void openSchemaObjectTab(const QString &objType);   /* selftest */
    void selftestUseDatabase(const QString &db);   /* --usedb= selftest */
    void selftestExpandDatabase(const QString &name);   /* --expandpgdb= selftest */
    void editTableCell(int row, int col, const QString &value, bool stageOnly = false);
    /* --dumpdb=FILE selftest: dump the active connection's default db */
    bool selftestDump(const QString &path);
    /* --copydb=src:tgt selftest */
    bool selftestCopyDb(const QString &src, const QString &tgt);
    /* --pgcopydb=src:tgt selftest (schema-to-schema, same connection) */
    bool selftestCopyDbPostgres(const QString &src, const QString &tgt);
    /* --pgmultidbtest=otherDb selftest: exercises ConnectionTab::
     * connectionFor()/paramsFor() — the side-connection plumbing behind
     * the Object Browser's multi-database tree */
    bool selftestMultiDb(const QString &otherDb);
    /* --sqlitecopydb=target[:nodata] selftest */
    bool selftestCopySqliteFile(const QString &target, bool withData);
    /* --sqlitecsvimport=file.csv:table / --pgcsvimport=file.csv:table[:onDup]
     * selftests (db is "" for SQLite, a schema for PostgreSQL) */
    bool selftestCsvImportBatched(const QString &db, const QString &file,
                                  const QString &table, const QString &onDup);
    /* --schemahtmltest=out.html selftest */
    bool selftestSchemaHtml(const QString &outFile);

private:
    ConnectionTab *currentTab() const;
    QAction *addDisabled(QMenu *menu, const QString &text);

    class QStackedWidget *m_stack = nullptr;
    QTabWidget *m_tabs    = nullptr;
    QComboBox  *m_dbCombo = nullptr;
    QLabel     *m_statusMsg        = nullptr;
    QLabel     *m_execLabel        = nullptr;
    QLabel     *m_totalLabel       = nullptr;
    QLabel     *m_cursorLabel      = nullptr;
    QLabel     *m_connectionsLabel = nullptr;
    QString     m_sessionFile;   /* File > Save/Open Session */
};
