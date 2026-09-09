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
    void editClipboard(const QString &what);
    void switchTab(int delta);

public:
    /* programmatic path used by the --autoconnect selftest (main.cpp) */
    bool openAndRun(const ConnectionParams &params);
    void openTableData(const QString &db, const QString &table);
    void editTableCell(int row, int col, const QString &value);

private:
    ConnectionTab *currentTab() const;
    QAction *addDisabled(QMenu *menu, const QString &text);

    QTabWidget *m_tabs    = nullptr;
    QComboBox  *m_dbCombo = nullptr;
    QLabel     *m_execLabel        = nullptr;
    QLabel     *m_cursorLabel      = nullptr;
    QLabel     *m_connectionsLabel = nullptr;
};
