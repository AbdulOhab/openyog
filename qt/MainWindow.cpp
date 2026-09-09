#include "MainWindow.h"
#include "ConnectionDialog.h"
#include "ConnectionStore.h"
#include "ConnectionTab.h"
#include "Theme.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("OpenYog"));
    resize(1200, 760);

    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    setCentralWidget(m_tabs);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [this](int) { syncToolbarToCurrentTab(); });

    /* ---- menus, mirroring SQLyog's structure ---------------------- */
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *newConn = file->addAction(QStringLiteral("&New Connection…"));
    newConn->setShortcut(QKeySequence::New);
    connect(newConn, &QAction::triggered, this, &MainWindow::newConnection);
    file->addSeparator();
    QAction *quit = file->addAction(QStringLiteral("E&xit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &MainWindow::close);

    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    for(const char *name : { "&Undo", "&Redo" })
        edit->addAction(QString::fromUtf8(name))->setEnabled(false);
    edit->addSeparator();
    for(const char *name : { "Cu&t", "&Copy", "&Paste", "Select &All" })
        edit->addAction(QString::fromUtf8(name))->setEnabled(false);

    QMenu *favorites = menuBar()->addMenu(QStringLiteral("&Favorites"));
    favorites->addAction(QStringLiteral("Organize &Favorites…"))->setEnabled(false);

    QMenu *database = menuBar()->addMenu(QStringLiteral("&Database"));
    QAction *refresh = database->addAction(QStringLiteral("&Refresh Objects"));
    refresh->setShortcut(QKeySequence(QStringLiteral("F5")));
    refresh->setEnabled(false);
    database->addAction(QStringLiteral("&Create Database…"))->setEnabled(false);
    database->addAction(QStringLiteral("&Drop Database…"))->setEnabled(false);

    QMenu *table = menuBar()->addMenu(QStringLiteral("&Table"));
    table->addAction(QStringLiteral("Create &Table…"))->setEnabled(false);
    table->addAction(QStringLiteral("&Open Table"))->setEnabled(false);

    QMenu *others = menuBar()->addMenu(QStringLiteral("&Others"));
    others->addAction(QStringLiteral("Copy Table…"))->setEnabled(false);

    QMenu *tools = menuBar()->addMenu(QStringLiteral("&Tools"));
    QAction *darkTheme = tools->addAction(QStringLiteral("&Dark Theme"));
    darkTheme->setCheckable(true);
    darkTheme->setChecked(Theme::load() == QStringLiteral("dark"));
    connect(darkTheme, &QAction::toggled, this, [this, darkTheme](bool checked) {
        const QString theme = checked ? QStringLiteral("dark")
                                      : QStringLiteral("light");
        Theme::save(theme);
        Theme::apply(*qApp, theme);
    });
    tools->addAction(QStringLiteral("&User Manager…"))->setEnabled(false);
    tools->addAction(QStringLiteral("&Backup…"))->setEnabled(false);

    QMenu *powertools = menuBar()->addMenu(QStringLiteral("&Powertools"));
    powertools->addAction(QStringLiteral("&Data Sync…"))->setEnabled(false);

    QMenu *transactions = menuBar()->addMenu(QStringLiteral("&Transactions"));
    transactions->addAction(QStringLiteral("&Start Transaction"))->setEnabled(false);

    QMenu *window = menuBar()->addMenu(QStringLiteral("&Window"));
    QAction *closeTabAct = window->addAction(QStringLiteral("&Close Tab"));
    closeTabAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(closeTabAct, &QAction::triggered, this, [this] {
        if(m_tabs->currentIndex() >= 0)
            closeTab(m_tabs->currentIndex());
    });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction *about = help->addAction(QStringLiteral("&About OpenYog"));
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::information(this, QStringLiteral("About OpenYog"),
            QStringLiteral("OpenYog — a cross-platform MySQL/MariaDB client.\n"
                           "GPL-3.0 fork of the SQLyog Community source.\n"
                           "Not affiliated with Webyog/Idera."));
    });

    /* ---- toolbar: actions + the database selector ----------------- */
    auto *toolbar = addToolBar(QStringLiteral("main"));
    toolbar->setMovable(false);
    toolbar->addAction(newConn);

    m_dbCombo = new QComboBox(toolbar);
    m_dbCombo->setMinimumContentsLength(22);
    toolbar->addWidget(m_dbCombo);
    connect(m_dbCombo, &QComboBox::activated, this,
            [this](int index) { useDatabaseFromCombo(m_dbCombo->itemText(index)); });

    /* ---- status bar: Ready | Exec | Total | Connections ----------- */
    auto *ready = new QLabel(QStringLiteral("Ready"), this);
    statusBar()->addWidget(ready, 1);
    m_execLabel = new QLabel(QStringLiteral("Exec: 0 sec"), this);
    m_connectionsLabel = new QLabel(QStringLiteral("Connections: 0"), this);
    statusBar()->addWidget(m_execLabel);
    statusBar()->addWidget(m_connectionsLabel);

    syncToolbarToCurrentTab();
}

ConnectionTab *MainWindow::currentTab() const
{
    return qobject_cast<ConnectionTab *>(m_tabs->currentWidget());
}

void MainWindow::syncToolbarToCurrentTab()
{
    auto *tab = currentTab();

    m_dbCombo->blockSignals(true);
    m_dbCombo->clear();
    if(tab) {
        m_dbCombo->addItems(tab->databases());
        m_dbCombo->setCurrentText(tab->currentDatabase());
    }
    m_dbCombo->blockSignals(false);

    if(tab) {
        setWindowTitle(QStringLiteral("OpenYog - [%1/%2 - %3]")
                           .arg(tab->title(), tab->currentDatabase(),
                                tab->hostLabel()));
        m_connectionsLabel->setText(
            QStringLiteral("Connections: %1").arg(m_tabs->count()));
    } else {
        setWindowTitle(QStringLiteral("OpenYog"));
        m_connectionsLabel->setText(QStringLiteral("Connections: 0"));
    }
}

void MainWindow::useDatabaseFromCombo(const QString &db)
{
    if(auto *tab = currentTab())
        tab->useDatabase(db);
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

    ConnectionStore::save(params);
    const int index = m_tabs->addTab(tab, tab->title());
    m_tabs->setCurrentIndex(index);

    /* tab → toolbar/status wiring (only reacts when this tab is current) */
    connect(tab, &ConnectionTab::databasesChanged, this,
            [this, tab](const QStringList &, const QString &) {
        if(m_tabs->currentWidget() == tab)
            syncToolbarToCurrentTab();
    });
    connect(tab, &ConnectionTab::executed, this, [this, tab](const QString &info) {
        if(m_tabs->currentWidget() == tab)
            m_execLabel->setText(info);
    });

    syncToolbarToCurrentTab();
    tab->runQuery();   /* run the editor's default query so the grid has data */
    return true;
}

void MainWindow::closeTab(int index)
{
    QWidget *w = m_tabs->widget(index);
    m_tabs->removeTab(index);
    delete w;
    syncToolbarToCurrentTab();
}
