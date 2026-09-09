/* OpenYog — main window with SQLyog's menu/toolbar/status-bar structure
 * (menus mirror upstream: File Edit Favorites Database Table Others Tools
 * Powertools Transactions Window Help). */
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

public:
    /* programmatic path used by the --autoconnect selftest (main.cpp) */
    bool openAndRun(const ConnectionParams &params);

private:
    ConnectionTab *currentTab() const;

    QTabWidget *m_tabs    = nullptr;
    QComboBox  *m_dbCombo = nullptr;
    QLabel     *m_execLabel       = nullptr;
    QLabel     *m_totalLabel      = nullptr;
    QLabel     *m_connectionsLabel = nullptr;
};
