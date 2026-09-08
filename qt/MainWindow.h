/* OpenYog — main window: menu/toolbar + one tab per server connection. */
#pragma once

#include <QMainWindow>

#include "ConnectionParams.h"

class QTabWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void newConnection();
    void closeTab(int index);

public:
    /* programmatic path used by the --autoconnect selftest (main.cpp) */
    bool openAndRun(const ConnectionParams &params);

private:
    QTabWidget *m_tabs = nullptr;
};
