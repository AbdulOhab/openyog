#include "MainWindow.h"
#include "ConnectionDialog.h"
#include "ConnectionStore.h"
#include "ConnectionTab.h"

#include <QAction>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("OpenYog — MySQL/MariaDB client"));
    resize(900, 600);

    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    setCentralWidget(m_tabs);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);

    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *newConn = file->addAction(QStringLiteral("&New Connection…"));
    newConn->setShortcut(QKeySequence::New);
    connect(newConn, &QAction::triggered, this, &MainWindow::newConnection);
    file->addSeparator();
    QAction *quit = file->addAction(QStringLiteral("E&xit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &MainWindow::close);

    auto *toolbar = addToolBar(QStringLiteral("main"));
    toolbar->setMovable(false);
    toolbar->addAction(newConn);

    statusBar()->showMessage(QStringLiteral(
        "Phase 2 skeleton — File > New Connection (Ctrl+N)"));
}

void MainWindow::newConnection()
{
    ConnectionDialog dlg(this);
    if(dlg.exec() != QDialog::Accepted)
        return;

    openAndRun(dlg.params());
}

bool MainWindow::openAndRun(const ConnectionParams &params)
{
    auto *tab = new ConnectionTab(params, this);
    if(!tab->isConnected()) {
        QMessageBox::warning(this, QStringLiteral("Connection failed"),
                             QStringLiteral("Could not connect to %1@%2:%3.")
                                 .arg(params.user, params.host)
                                 .arg(params.port));
        delete tab;
        return false;
    }

    ConnectionStore::save(params);   /* remember for next time + prefill */
    const int index = m_tabs->addTab(tab, tab->title());
    m_tabs->setCurrentIndex(index);
    statusBar()->showMessage(QStringLiteral("Connected: %1").arg(tab->title()));

    tab->runQuery();   /* run the editor's default query so the grid has data */
    return true;
}

void MainWindow::closeTab(int index)
{
    QWidget *w = m_tabs->widget(index);
    m_tabs->removeTab(index);
    delete w;
}
