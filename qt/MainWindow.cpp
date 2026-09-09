/* OpenYog — main window.
 * The menus are transcribed from the upstream resource script
 * (include/SQLyog.rc, IDR_MAINMENU): labels, nesting and shortcuts are
 * verbatim. Items whose functionality hasn't been ported yet stay visible but
 * disabled — they mark the roadmap (FEATURES.md); working items are wired. */
#include "MainWindow.h"
#include "ConnectionDialog.h"
#include "ConnectionStore.h"
#include "ConnectionTab.h"
#include "Icons.h"
#include "Theme.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QIcon>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QFrame>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>

namespace {
QAction *addDisabled(QMenu *menu, const QString &text)
{
    QAction *a = menu->addAction(text);
    a->setEnabled(false);
    return a;
}

/* apply clipboard/undo/redo/case ops to whatever editor has focus */
QPlainTextEdit *focusedEditor()
{
    return qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("OpenYog"));
    resize(1200, 760);

    /* app icon at the left of the menu bar — visible regardless of whether the
     * window manager draws a title-bar icon (SQLyog shows one here too) */
    auto *menuIcon = new QLabel(this);
    menuIcon->setPixmap(QIcon(QStringLiteral(":/resources/openyog-16.png"))
                            .pixmap(16, 16));
    menuIcon->setContentsMargins(6, 0, 4, 0);
    menuBar()->setCornerWidget(menuIcon, Qt::TopLeftCorner);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("connTabs"));
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setExpanding(false);        /* SQLyog left-aligns tabs */
    auto *plus = new QPushButton(QStringLiteral("+"), this);
    plus->setFixedSize(22, 20);
    plus->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #3B7DBB; "
        "border: none; font-weight: bold; }"
        "QPushButton:hover { background: #E8F2FA; }"));
    connect(plus, &QPushButton::clicked, this, &MainWindow::newConnection);
    m_tabs->setCornerWidget(plus, Qt::TopRightCorner);
    setCentralWidget(m_tabs);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [this](int) { syncToolbarToCurrentTab(); });

    /* ================= File (IDR_MAINMENU) ========================== */
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *newSame = file->addAction(
        QStringLiteral("New Connection Using Current Settings\tCtrl+N"));
    connect(newSame, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            openAndRun([&] {
                ConnectionParams p;
                p.host     = t->property("host").toString();
                p.port     = t->property("port").toInt();
                p.user     = t->property("user").toString();
                p.password = t->property("password").toString();
                p.database = t->property("database").toString();
                p.name     = t->title();
                return p;
            }());
    });
    QAction *newConn = file->addAction(QStringLiteral("New &Connection…\tCtrl+M"));
    connect(newConn, &QAction::triggered, this, &MainWindow::newConnection);
    file->addSeparator();
    addDisabled(file, QStringLiteral("New &Query Editor\tCtrl+T"));
    addDisabled(file, QStringLiteral("New Query &Builder\tCtrl+K"));
    addDisabled(file, QStringLiteral("Ne&w Schema Designer\tCtrl+Alt+D"));
    addDisabled(file, QStringLiteral("New Data Searc&h\tCtrl+Shift+D"));
    file->addSeparator();
    QAction *closeTabAct = file->addAction(QStringLiteral("Close &Tab\tAlt+L"));
    connect(closeTabAct, &QAction::triggered, this, [this] {
        if(m_tabs->currentIndex() >= 0)
            closeTab(m_tabs->currentIndex());
    });
    addDisabled(file, QStringLiteral("&Rename Tab\tAlt+F2"));
    QAction *disconnect = file->addAction(QStringLiteral("&Disconnect\tCtrl+F4"));
    connect(disconnect, &QAction::triggered, this, [this] {
        if(m_tabs->currentIndex() >= 0)
            closeTab(m_tabs->currentIndex());
    });
    QAction *disconnectAll = file->addAction(QStringLiteral("Disconnect Al&l"));
    connect(disconnectAll, &QAction::triggered, this, [this] {
        while(m_tabs->count())
            closeTab(0);
    });
    file->addSeparator();
    QAction *openSql = file->addAction(QStringLiteral("Open…\tCtrl+O"));
    connect(openSql, &QAction::triggered, this, [this] {
        if(auto *tab = currentTab()) {
            const QString f = QFileDialog::getOpenFileName(
                this, QStringLiteral("Open SQL file"), QString(),
                QStringLiteral("SQL (*.sql);;All (*)"));
            if(!f.isEmpty())
                tab->openSqlFile(f);
        }
    });
    QAction *saveSql = file->addAction(QStringLiteral("&Save…\tCtrl+S"));
    connect(saveSql, &QAction::triggered, this, [this] {
        if(auto *tab = currentTab())
            tab->saveEditor();
    });
    addDisabled(file, QStringLiteral("S&ave As…"));
    file->addSeparator();
    addDisabled(file, QStringLiteral("Open Session Savepoint…\tCtrl+Shift+O"));
    addDisabled(file, QStringLiteral("Save Session…\tCtrl+Shift+S"));
    addDisabled(file, QStringLiteral("Save Session As…"));
    addDisabled(file, QStringLiteral("End Session\tCtrl+Shift+X"));
    file->addSeparator();
    QMenu *recent = file->addMenu(QStringLiteral("&Recent Files"));
    recent->addAction(QStringLiteral("(no recent files)"))->setEnabled(false);
    file->addSeparator();
    QAction *quit = file->addAction(QStringLiteral("E&xit\tAlt+F4"));
    connect(quit, &QAction::triggered, this, &MainWindow::close);

    /* ================= Edit ========================================= */
    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    QAction *refresh = edit->addAction(QStringLiteral("Refresh &Object Browser\tF5"));
    connect(refresh, &QAction::triggered, this, &MainWindow::refreshBrowser);
    addDisabled(edit, QStringLiteral("Change Objec&t Browser Color"));
    addDisabled(edit, QStringLiteral("Collapse All in Object Browser\tShift+{-}"));
    edit->addSeparator();
    QMenu *execMenu = edit->addMenu(QStringLiteral("Execute Quer&y"));
    QAction *execQuery = execMenu->addAction(QStringLiteral("Exe&cute Query\tF9"));
    connect(execQuery, &QAction::triggered, this, &MainWindow::executeCurrentTab);
    QAction *execAll = execMenu->addAction(QStringLiteral("Execute &All Queries\tCtrl+F9"));
    connect(execAll, &QAction::triggered, this, &MainWindow::executeCurrentTab);
    addDisabled(execMenu, QStringLiteral("Execute And Edit &Resultset\tF8"));
    QMenu *explain = edit->addMenu(QStringLiteral("Execute Explain"));
    addDisabled(explain, QStringLiteral("EXPLAIN <Query>"));
    addDisabled(explain, QStringLiteral("EXPLAIN EXTENDED <Query>"));
    edit->addSeparator();
    QMenu *formatter = edit->addMenu(QStringLiteral("S&QL Formatter"));
    addDisabled(formatter, QStringLiteral("Format &Current Query\tF12"));
    addDisabled(formatter, QStringLiteral("Format &Selected Query\tCtrl+F12"));
    addDisabled(formatter, QStringLiteral("Format &All Queries\tShift+F12"));
    addDisabled(edit, QStringLiteral("&Insert Templates…\tCtrl+Shift+T"));
    edit->addSeparator();
    QAction *undo = edit->addAction(QStringLiteral("&Undo\tCtrl+Z"));
    connect(undo, &QAction::triggered, this,
            [this] { editClipboard(QStringLiteral("undo")); });
    QAction *redo = edit->addAction(QStringLiteral("&Redo\tCtrl+Y"));
    connect(redo, &QAction::triggered, this,
            [this] { editClipboard(QStringLiteral("redo")); });
    edit->addSeparator();
    QAction *cut = edit->addAction(QStringLiteral("Cu&t\tCtrl+X"));
    connect(cut, &QAction::triggered, this,
            [this] { editClipboard(QStringLiteral("cut")); });
    QAction *copy = edit->addAction(QStringLiteral("&Copy\tCtrl+C"));
    connect(copy, &QAction::triggered, this,
            [this] { editClipboard(QStringLiteral("copy")); });
    addDisabled(edit, QStringLiteral("Copy With Normalized &Whitespace\tAlt+C"));
    QAction *paste = edit->addAction(QStringLiteral("&Paste\tCtrl+V"));
    connect(paste, &QAction::triggered, this,
            [this] { editClipboard(QStringLiteral("paste")); });
    addDisabled(edit, QStringLiteral("Insert From Fi&le…"));
    QAction *selAll = edit->addAction(QStringLiteral("Select &All\tCtrl+A"));
    connect(selAll, &QAction::triggered, this,
            [this] { editClipboard(QStringLiteral("selectall")); });
    edit->addSeparator();
    addDisabled(edit, QStringLiteral("&Find…\tCtrl+F"));
    addDisabled(edit, QStringLiteral("Find Next\tF3"));
    addDisabled(edit, QStringLiteral("R&eplace…\tCtrl+H"));
    addDisabled(edit, QStringLiteral("&Go To…\tCtrl+G"));
    edit->addSeparator();
    addDisabled(edit, QStringLiteral("Li&st All Tags\tCtrl+Space"));
    addDisabled(edit, QStringLiteral("List &Matching Tags\tCtrl+Enter"));
    edit->addSeparator();
    QAction *hideBrowser = edit->addAction(
        QStringLiteral("Hide Object &Browser\tCtrl+Shift+1"));
    connect(hideBrowser, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->toggleBrowserPane();
    });
    QAction *hideResult = edit->addAction(
        QStringLiteral("Hide Result Pa&ne\tCtrl+Shift+2"));
    connect(hideResult, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->toggleResultPane();
    });
    QAction *hideEditor = edit->addAction(
        QStringLiteral("&Hide SQL Editor\tCtrl+Shift+3"));
    connect(hideEditor, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->toggleEditorPane();
    });
    QAction *prevTab = edit->addAction(
        QStringLiteral("Switch To Pre&vious Tab\tCtrl+PgUp"));
    connect(prevTab, &QAction::triggered, this, [this] { switchTab(-1); });
    QAction *nextTab = edit->addAction(
        QStringLiteral("Switch To Ne&xt Tab\tCtrl+PgDown"));
    connect(nextTab, &QAction::triggered, this, [this] { switchTab(1); });
    edit->addSeparator();
    QMenu *advanced = edit->addMenu(QStringLiteral("A&dvanced"));
    QAction *upper = advanced->addAction(
        QStringLiteral("Make Selection &Uppercase\tCtrl+Shift+U"));
    connect(upper, &QAction::triggered, this,
            [this] { editClipboard(QStringLiteral("upper")); });
    QAction *lower = advanced->addAction(
        QStringLiteral("Make Selection &Lowercase\tCtrl+Shift+L"));
    connect(lower, &QAction::triggered, this,
            [this] { editClipboard(QStringLiteral("lower")); });
    advanced->addSeparator();
    addDisabled(advanced, QStringLiteral("&Comment Selection\tCtrl+Shift+C"));
    addDisabled(advanced, QStringLiteral("&Remove Comment From Selection\tCtrl+Shift+R"));

    /* ================= Favorites ==================================== */
    QMenu *favorites = menuBar()->addMenu(QStringLiteral("Fa&vorites"));
    addDisabled(favorites, QStringLiteral("&Add To Favorites…\tCtrl+Shift+F"));
    addDisabled(favorites, QStringLiteral("&Organize Favorites…"));
    addDisabled(favorites, QStringLiteral("&Refresh Favorites"));

    /* ================= Database ===================================== */
    QMenu *database = menuBar()->addMenu(QStringLiteral("&Database"));
    addDisabled(database,
        QStringLiteral("&Copy Database To Different Host/Database…"));
    QAction *createDb = database->addAction(
        QStringLiteral("Create &Database…\tCtrl+D"));
    connect(createDb, &QAction::triggered, this, &MainWindow::createDatabase);
    addDisabled(database, QStringLiteral("&Alter Database…\tF6"));
    QMenu *create = database->addMenu(QStringLiteral("C&reate"));
    QAction *createTblFromDb = create->addAction(QStringLiteral("&Table"));
    connect(createTblFromDb, &QAction::triggered, this,
            [this] { createTable(); });
    addDisabled(create, QStringLiteral("&View…"));
    addDisabled(create, QStringLiteral("&Stored Procedure…"));
    addDisabled(create, QStringLiteral("&Function…"));
    addDisabled(create, QStringLiteral("Tri&gger…"));
    addDisabled(create, QStringLiteral("&Event…"));
    QMenu *dbOps = database->addMenu(QStringLiteral("More Database &Operations"));
    addDisabled(dbOps, QStringLiteral("Dro&p Database…\tDel"));
    addDisabled(dbOps, QStringLiteral("Tr&uncate Database…\tShift+Del"));
    addDisabled(dbOps, QStringLiteral("E&mpty Database…"));
    database->addSeparator();
    QMenu *dbBackup = database->addMenu(QStringLiteral("&Backup/Export"));
    addDisabled(dbBackup, QStringLiteral("&Scheduled Backups…\tCtrl+Alt+S"));
    QAction *dbDump = dbBackup->addAction(
        QStringLiteral("&Backup Database As SQL Dump…\tCtrl+Alt+E"));
    connect(dbDump, &QAction::triggered, this, [this] { dumpDatabase(); });
    QMenu *dbImport = database->addMenu(QStringLiteral("&Import "));
    addDisabled(dbImport, QStringLiteral("Import E&xternal Data…\tCtrl+Alt+O"));
    addDisabled(dbImport, QStringLiteral("&Execute SQL Script…\tCtrl+Shift+Q"));
    database->addSeparator();
    addDisabled(database,
        QStringLiteral("Create Schema For Database In &HTML…\tCtrl+Shift+Alt+S"));

    /* ================= Table ======================================== */
    QMenu *table = menuBar()->addMenu(QStringLiteral("T&able"));
    QMenu *pasteSql = table->addMenu(QStringLiteral("&Paste SQL Statement"));
    QAction *pasteIns = pasteSql->addAction(
        QStringLiteral("&INSERT INTO <tablename>…\tAlt+Shift+I"));
    QAction *pasteUpd = pasteSql->addAction(
        QStringLiteral("&UPDATE <tablename> SET…\tAlt+Shift+U"));
    QAction *pasteDel = pasteSql->addAction(
        QStringLiteral("&DELETE FROM <tablename>…\tAlt+Shift+D"));
    QAction *pasteSel = pasteSql->addAction(
        QStringLiteral("&SELECT <col-1>…<col-n> FROM…\tAlt+Shift+S"));
    connect(pasteIns, &QAction::triggered, this,
            [this] { if(auto *t = currentTab()) t->pasteSqlTemplate(0); });
    connect(pasteUpd, &QAction::triggered, this,
            [this] { if(auto *t = currentTab()) t->pasteSqlTemplate(1); });
    connect(pasteDel, &QAction::triggered, this,
            [this] { if(auto *t = currentTab()) t->pasteSqlTemplate(2); });
    connect(pasteSel, &QAction::triggered, this,
            [this] { if(auto *t = currentTab()) t->pasteSqlTemplate(3); });
    addDisabled(table,
        QStringLiteral("&Copy Table(s) To Different Host/Database…"));
    table->addSeparator();
    QAction *openTable = table->addAction(QStringLiteral("&Open Table\tF11"));
    connect(openTable, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->openSelectedTable();
    });
    addDisabled(table, QStringLiteral("Open Table in &New Tab\tCtrl+F11"));
    QAction *createTbl = table->addAction(QStringLiteral("Create &Table\tF4"));
    createTbl->setShortcut(QKeySequence(Qt::Key_F4));
    connect(createTbl, &QAction::triggered, this, [this] { createTable(); });
    QAction *alterTbl = table->addAction(QStringLiteral("&Alter Table\tF6"));
    alterTbl->setShortcut(QKeySequence(Qt::Key_F6));
    connect(alterTbl, &QAction::triggered, this, &MainWindow::alterTable);
    /* each of these acts on the object browser's selected table */
    const auto onSelectedTable = [this](void (ConnectionTab::*fn)(const QString &,
                                                                  const QString &)) {
        auto *tab = currentTab();
        if(!tab) { return; }
        const QStringList info = tab->selectedTableInfo();
        if(info.size() < 2) {
            QMessageBox::information(this, QStringLiteral("OpenYog"),
                QStringLiteral("Select a table in the object browser first."));
            return;
        }
        (tab->*fn)(info[0], info[1]);
    };
    QAction *manageIdx = table->addAction(QStringLiteral("&Manage Indexes\tF7"));
    manageIdx->setShortcut(QKeySequence(Qt::Key_F7));
    connect(manageIdx, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptManageIndexes); });
    QAction *manageFk = table->addAction(
        QStringLiteral("Re&lationships/Foreign Keys\tF10"));
    manageFk->setShortcut(QKeySequence(Qt::Key_F10));
    connect(manageFk, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptManageForeignKeys); });
    QMenu *moreTable = table->addMenu(QStringLiteral("Mo&re Table Operations"));
    QAction *renameTbl = moreTable->addAction(QStringLiteral("&Rename Table\tF2"));
    renameTbl->setShortcut(QKeySequence(Qt::Key_F2));
    connect(renameTbl, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptRenameTable); });
    QAction *truncTbl = moreTable->addAction(QStringLiteral("Tru&ncate Table…\tShift+Del"));
    connect(truncTbl, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::truncateTable); });
    QAction *dropTbl = moreTable->addAction(QStringLiteral("&Drop Table From Database…\tDel"));
    connect(dropTbl, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::dropTable); });
    addDisabled(moreTable, QStringLiteral("Re&order Column(s)\tCtrl+Alt+R"));
    QAction *dupTbl = moreTable->addAction(
        QStringLiteral("Duplicate Table &Structure/Data…"));
    connect(dupTbl, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptCopyTable); });
    addDisabled(moreTable, QStringLiteral("View &Table Properties"));
    table->addSeparator();
    QMenu *tblBackup = table->addMenu(QStringLiteral("&Backup/Export"));
    addDisabled(tblBackup, QStringLiteral("&Scheduled Backups…\tCtrl+Alt+S"));
    addDisabled(tblBackup, QStringLiteral("&Backup Table(s) As SQL Dump…\tCtrl+Alt+E"));
    addDisabled(tblBackup, QStringLiteral("&Export Table Data As…\tCtrl+Alt+C"));
    QMenu *tblImport = table->addMenu(QStringLiteral("&Import "));
    addDisabled(tblImport, QStringLiteral("Import E&xternal Data…\tCtrl+Alt+O"));
    addDisabled(tblImport,
        QStringLiteral("&Import CSV Data Using LOAD LOCAL…\tCtrl+Shift+M"));
    addDisabled(tblImport,
        QStringLiteral("Import &XML Data Using LOAD LOCAL…\tCtrl+Shift+X"));
    table->addSeparator();
    addDisabled(table, QStringLiteral("Create Tri&gger…"));

    /* ================= Others ======================================= */
    QMenu *others = menuBar()->addMenu(QStringLiteral("&Others"));
    QMenu *columns = others->addMenu(QStringLiteral("&Columns"));
    addDisabled(columns, QStringLiteral("Drop &Column…\tDel"));
    addDisabled(columns, QStringLiteral("&Manage Columns\tF6"));
    QMenu *indexes = others->addMenu(QStringLiteral("&Indexes"));
    addDisabled(indexes, QStringLiteral("Create &Index\tF4"));
    addDisabled(indexes, QStringLiteral("&Edit Index\tF6"));

    /* ================= Tools ======================================== */
    QMenu *tools = menuBar()->addMenu(QStringLiteral("&Tools"));
    QAction *exportRows = tools->addAction(
        QStringLiteral("&Export All Rows Of Table Data/Result As…\tCtrl+Shift+E"));
    connect(exportRows, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->exportResultCsv();
    });
    QAction *toolsDump = tools->addAction(
        QStringLiteral("&Backup Database As SQL Dump…\tCtrl+Alt+E"));
    connect(toolsDump, &QAction::triggered, this, [this] { dumpDatabase(); });
    QAction *runScript = tools->addAction(
        QStringLiteral("Execute &SQL Script…\tCtrl+Shift+Q"));
    connect(runScript, &QAction::triggered, this, [this] {
        if(auto *t = currentTab()) {
            const QString f = QFileDialog::getOpenFileName(
                this, QStringLiteral("Execute SQL script"), QString(),
                QStringLiteral("SQL (*.sql);;All (*)"));
            if(!f.isEmpty())
                t->openSqlFile(f);   /* loads, so the user sees what runs */
        }
    });
    tools->addSeparator();
    addDisabled(tools, QStringLiteral("&Flush…\tCtrl+Alt+F"));
    addDisabled(tools, QStringLiteral("&Table Diagnostics…\tCtrl+Alt+T"));
    QAction *history = tools->addAction(QStringLiteral("&History\tCtrl+Shift+H"));
    connect(history, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->showHistory();
    });
    addDisabled(tools, QStringLiteral("&Info\tCtrl+Shift+I"));
    tools->addSeparator();
    addDisabled(tools, QStringLiteral("&User Manager\tCtrl+U"));
    QMenu *show = tools->addMenu(QStringLiteral("Sho&w"));
    QAction *showVars = show->addAction(QStringLiteral("&Variables…"));
    connect(showVars, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->runStatements(QStringList{ QStringLiteral("SHOW VARIABLES") },
                             QStringLiteral("Variables"));
    });
    QAction *showProc = show->addAction(QStringLiteral("&Processlist…"));
    connect(showProc, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->runStatements(QStringList{ QStringLiteral("SHOW FULL PROCESSLIST") },
                             QStringLiteral("Processlist"));
    });
    QAction *showStatus = show->addAction(QStringLiteral("&Status…"));
    connect(showStatus, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->runStatements(QStringList{ QStringLiteral("SHOW STATUS") },
                             QStringLiteral("Status"));
    });
    tools->addSeparator();
    addDisabled(tools, QStringLiteral("Change &Language\tAlt+Shift+L"));
    QMenu *connDetails =
        tools->addMenu(QStringLiteral("Export/I&mport Connection Details"));
    addDisabled(connDetails, QStringLiteral("&Export Connection Details…"));
    addDisabled(connDetails, QStringLiteral("&Import Connection Details…"));
    addDisabled(tools, QStringLiteral("&Preferences…"));
    QAction *darkTheme = tools->addAction(QStringLiteral("&Dark Theme"));
    darkTheme->setCheckable(true);
    darkTheme->setChecked(Theme::load() == QStringLiteral("dark"));
    connect(darkTheme, &QAction::toggled, this, [](bool checked) {
        const QString theme = checked ? QStringLiteral("dark")
                                      : QStringLiteral("light");
        Theme::save(theme);
        Theme::apply(*qApp, theme);
    });

    /* ================= Powertools =================================== */
    QMenu *powertools = menuBar()->addMenu(QStringLiteral("&Powertools"));
    addDisabled(powertools,
        QStringLiteral("Database S&ynchronization Wizard…\tCtrl+Alt+W"));
    addDisabled(powertools,
        QStringLiteral("&Visual Data Comparison Wizard…\tCtrl+Alt+Q"));
    addDisabled(powertools, QStringLiteral("Schema &Synchronization Tool…\tCtrl+Q"));
    powertools->addSeparator();
    addDisabled(powertools, QStringLiteral("Import E&xternal Data…\tCtrl+Alt+O"));
    addDisabled(powertools,
        QStringLiteral("S&QL Scheduler and Reporting Wizard…\tCtrl+Alt+N"));
    addDisabled(powertools, QStringLiteral("S&cheduled Backups…\tCtrl+Alt+S"));
    powertools->addSeparator();
    addDisabled(powertools, QStringLiteral("Scheduled &Jobs…"));
    powertools->addSeparator();
    addDisabled(powertools, QStringLiteral("&Rebuild tags"));

    /* ================= Transactions ================================= */
    QMenu *transactions = menuBar()->addMenu(QStringLiteral("T&ransactions"));
    addDisabled(transactions, QStringLiteral("Set Autocommit"));
    QMenu *isolation = transactions->addMenu(QStringLiteral("Isolation Level"));
    addDisabled(isolation, QStringLiteral("Repeatable Read"));
    addDisabled(isolation, QStringLiteral("Read Committed"));
    addDisabled(isolation, QStringLiteral("Read Uncommitted"));
    addDisabled(isolation, QStringLiteral("Serializable"));
    transactions->addSeparator();
    QMenu *startTrx = transactions->addMenu(QStringLiteral("Start Transaction"));
    addDisabled(startTrx, QStringLiteral("With no modifier"));
    QMenu *snapshot = startTrx->addMenu(QStringLiteral("With Consistent Snapshot"));
    addDisabled(snapshot, QStringLiteral("Read only"));
    addDisabled(snapshot, QStringLiteral("Read Write"));
    QMenu *commit = transactions->addMenu(QStringLiteral("Commit"));
    addDisabled(commit, QStringLiteral("With no modifier"));
    commit->addSeparator();
    addDisabled(commit, QStringLiteral("And Chain"));
    addDisabled(commit, QStringLiteral("And No Chain"));
    commit->addSeparator();
    addDisabled(commit, QStringLiteral("Release"));
    addDisabled(commit, QStringLiteral("No Release"));
    QMenu *rollback = transactions->addMenu(QStringLiteral("Rollback"));
    addDisabled(rollback, QStringLiteral("To Savepoint"));
    addDisabled(rollback, QStringLiteral("Transaction"));

    /* ================= Window ======================================= */
    QMenu *window = menuBar()->addMenu(QStringLiteral("&Window"));
    QAction *winClose = window->addAction(QStringLiteral("&Close Tab\tAlt+L"));
    connect(winClose, &QAction::triggered, this, [this] {
        if(m_tabs->currentIndex() >= 0)
            closeTab(m_tabs->currentIndex());
    });
    QAction *winNext = window->addAction(QStringLiteral("&Next Tab\tCtrl+PgDown"));
    connect(winNext, &QAction::triggered, this, [this] { switchTab(1); });
    QAction *winPrev = window->addAction(QStringLiteral("&Previous Tab\tCtrl+PgUp"));
    connect(winPrev, &QAction::triggered, this, [this] { switchTab(-1); });

    /* ================= Help ========================================= */
    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction *about = help->addAction(QStringLiteral("&About OpenYog"));
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::information(this, QStringLiteral("About OpenYog"),
            QStringLiteral("OpenYog — a cross-platform MySQL/MariaDB client.\n"
                           "GPL-3.0 fork of the SQLyog Community source.\n"
                           "Not affiliated with Webyog/Idera."));
    });

    /* ================= toolbar + status bar ========================= */
    /* button order + icons follow the SQLyog main toolbar (IDR_MAINFRAME) */
    auto *toolbar = addToolBar(QStringLiteral("main"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(16, 16));

    newConn->setIcon(Icons::get(QStringLiteral("connect_16.ico")));
    toolbar->addAction(newConn);
    toolbar->addSeparator();

    QAction *execTool = toolbar->addAction(
        Icons::get(QStringLiteral("execute_16.ico")),
        QStringLiteral("Execute Query\tF9"));
    connect(execTool, &QAction::triggered, this, &MainWindow::executeCurrentTab);
    QAction *execAllTool = toolbar->addAction(
        Icons::get(QStringLiteral("execall_16.ico")),
        QStringLiteral("Execute All Queries\tCtrl+F9"));
    connect(execAllTool, &QAction::triggered, this, &MainWindow::executeCurrentTab);
    QAction *execEditTool = toolbar->addAction(
        Icons::get(QStringLiteral("execforupd_16.ico")),
        QStringLiteral("Execute Query & Edit Resultset\tF8"));
    execEditTool->setEnabled(false);
    QAction *stopTool = toolbar->addAction(
        Icons::get(QStringLiteral("Stop_16.ico")), QStringLiteral("Stop"));
    stopTool->setEnabled(false);
    toolbar->addSeparator();

    QAction *refreshTool = toolbar->addAction(
        Icons::get(QStringLiteral("refresh_16.ico")),
        QStringLiteral("Refresh Object Browser\tF5"));
    connect(refreshTool, &QAction::triggered, this, &MainWindow::refreshBrowser);
    QAction *formatTool = toolbar->addAction(
        Icons::get(QStringLiteral("formatall.ico")),
        QStringLiteral("Format All Queries\tShift+F12"));
    formatTool->setEnabled(false);
    toolbar->addSeparator();

    m_dbCombo = new QComboBox(toolbar);
    m_dbCombo->setMinimumContentsLength(22);
    toolbar->addWidget(m_dbCombo);
    connect(m_dbCombo, &QComboBox::activated, this,
            [this](int index) { useDatabaseFromCombo(m_dbCombo->itemText(index)); });
    toolbar->addSeparator();

    QAction *userMgrTool = toolbar->addAction(
        Icons::get(QStringLiteral("usermanager.ICO")),
        QStringLiteral("User Manager\tCtrl+U"));
    userMgrTool->setEnabled(false);

    m_statusMsg = new QLabel(QStringLiteral("Ready"), this);
    statusBar()->addWidget(m_statusMsg, 1);
    m_execLabel = new QLabel(QStringLiteral("Exec: 0 sec"), this);
    m_totalLabel = new QLabel(QStringLiteral("Total: 0 sec"), this);
    m_cursorLabel = new QLabel(QStringLiteral("Ln 1, Col 1"), this);
    m_connectionsLabel = new QLabel(QStringLiteral("Connections: 0"), this);
    for(QLabel *l : { m_execLabel, m_totalLabel, m_cursorLabel, m_connectionsLabel }) {
        l->setMinimumWidth(90);
        l->setFrameStyle(QFrame::Panel | QFrame::Sunken);
        statusBar()->addPermanentWidget(l);
    }

    syncToolbarToCurrentTab();
}

QAction *MainWindow::addDisabled(QMenu *menu, const QString &text)
{
    QAction *a = menu->addAction(text);
    a->setEnabled(false);
    return a;
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

void MainWindow::executeCurrentTab()
{
    if(auto *tab = currentTab())
        tab->runQuery();
}

void MainWindow::refreshBrowser()
{
    if(auto *tab = currentTab())
        tab->refreshBrowser();
}

void MainWindow::createDatabase()
{
    auto *tab = currentTab();
    if(!tab)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Create Database"),
        QStringLiteral("Database name:"), QLineEdit::Normal, {}, &ok);
    if(!ok || name.isEmpty())
        return;
    tab->execDdl(QStringLiteral("CREATE DATABASE `%1` CHARACTER SET utf8mb4")
                     .arg(name));
}

void MainWindow::createTable(const QString &database)
{
    if(auto *tab = currentTab())
        tab->promptCreateTable(database);
    else
        QMessageBox::information(this, QStringLiteral("Create Table"),
            QStringLiteral("Open a connection first."));
}

void MainWindow::alterTable()
{
    auto *tab = currentTab();
    if(!tab) {
        QMessageBox::information(this, QStringLiteral("Alter Table"),
            QStringLiteral("Open a connection first."));
        return;
    }
    const QStringList info = tab->selectedTableInfo();
    if(info.size() < 2) {
        QMessageBox::information(this, QStringLiteral("Alter Table"),
            QStringLiteral("Select a table in the object browser first."));
        return;
    }
    tab->promptAlterTable(info[0], info[1]);
}

void MainWindow::dumpDatabase(const QString &database)
{
    if(auto *tab = currentTab())
        tab->promptDumpDatabase(database);
    else
        QMessageBox::information(this, QStringLiteral("Backup As SQL Dump"),
            QStringLiteral("Open a connection first."));
}

void MainWindow::editClipboard(const QString &what)
{
    auto *ed = focusedEditor();
    if(!ed)
        return;
    if(what == QStringLiteral("undo"))           ed->undo();
    else if(what == QStringLiteral("redo"))      ed->redo();
    else if(what == QStringLiteral("cut"))       ed->cut();
    else if(what == QStringLiteral("copy"))      ed->copy();
    else if(what == QStringLiteral("paste"))     ed->paste();
    else if(what == QStringLiteral("selectall")) ed->selectAll();
    else if(what == QStringLiteral("upper") || what == QStringLiteral("lower")) {
        QTextCursor c = ed->textCursor();
        const QString sel = c.selectedText();
        if(!sel.isEmpty())
            c.insertText(what == QStringLiteral("upper") ? sel.toUpper()
                                                         : sel.toLower());
    }
}

void MainWindow::switchTab(int delta)
{
    const int n = m_tabs->count();
    if(!n)
        return;
    m_tabs->setCurrentIndex((m_tabs->currentIndex() + delta + n) % n);
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

    /* stash connection info for "New Connection Using Current Settings" */
    tab->setProperty("host", params.host);
    tab->setProperty("port", params.port);
    tab->setProperty("user", params.user);
    tab->setProperty("password", params.password);
    tab->setProperty("database", params.database);

    connect(tab, &ConnectionTab::databasesChanged, this,
            [this, tab](const QStringList &, const QString &) {
        if(m_tabs->currentWidget() == tab)
            syncToolbarToCurrentTab();
    });
    connect(tab, &ConnectionTab::executed, this, [this, tab](const QString &info) {
        if(m_tabs->currentWidget() != tab)
            return;
        /* ConnectionTab emits "Exec: X sec | Total: Y sec" */
        const QStringList parts = info.split(QStringLiteral(" | "));
        m_execLabel->setText(parts.value(0, QStringLiteral("Exec: 0 sec")));
        m_totalLabel->setText(parts.value(1, QStringLiteral("Total: 0 sec")));
    });
    connect(tab, &ConnectionTab::cursorMoved, this,
            [this, tab](const QString &pos) {
        if(m_tabs->currentWidget() == tab)
            m_cursorLabel->setText(pos);
    });

    syncToolbarToCurrentTab();
    tab->runQuery();   /* run the editor's default query so the grid has data */
    return true;
}

void MainWindow::editTableCell(int row, int col, const QString &value, bool stageOnly)
{
    if(auto *tab = currentTab())
        tab->editTableCell(row, col, value, stageOnly);
}

bool MainWindow::selftestDump(const QString &path)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    QString err;
    const bool ok = tab->dumpDatabaseToFile({}, path, &err);
    if(!ok)
        qWarning("dump failed: %s", qPrintable(err));
    return ok;
}

void MainWindow::openTableData(const QString &db, const QString &table)
{
    if(auto *tab = currentTab())
        tab->openTableData(db, table);
}

void MainWindow::closeTab(int index)
{
    QWidget *w = m_tabs->widget(index);
    m_tabs->removeTab(index);
    delete w;
    syncToolbarToCurrentTab();
}
