/* OpenYog — main window: the connection tabs, the welcome page, and the
 * actions the menus and toolbars call. The menus themselves are built in
 * MainWindowMenus.cpp and MainWindowToolsMenus.cpp; the selftest hooks live
 * in MainWindowSelftest.cpp. */
#include "MainWindow.h"
#include "DataSyncDialog.h"
#include "SchemaSyncDialog.h"
#include "ConnectionDialog.h"
#include "ConnectionStore.h"
#include "ConnectionTab.h"
#include "Icons.h"
#include "MongoTab.h"
#include "ObjectBrowser.h"
#include "SessionStore.h"
#include "Theme.h"
#include "db/MongoClient.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QPixmap>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCloseEvent>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QVBoxLayout>

namespace {
/* apply clipboard/undo/redo/case ops to whatever editor has focus */
QPlainTextEdit *focusedEditor()
{
    return qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("OpenYog"));
    resize(1200, 760);

    /* unsaved queries survive a crash too, not just a clean quit */
    auto *autosave = new QTimer(this);
    autosave->setInterval(60 * 1000);
    connect(autosave, &QTimer::timeout, this, &MainWindow::saveSessionState);
    autosave->start();

    /* app icon at the left of the menu bar — visible regardless of whether the
     * window manager draws a title-bar icon (SQLyog shows one here too).
     * 16px, matching the 9pt menu text height: a 24px pixmap here grew the
     * whole bar (the corner widget sets its height) and dwarfed the labels */
    auto *menuIcon = new QLabel(this);
    menuIcon->setPixmap(QPixmap(QStringLiteral(":/resources/openyog-16.png")));
    menuIcon->setContentsMargins(12, 1, 8, 1);
    menuBar()->setCornerWidget(menuIcon, Qt::TopLeftCorner);
    /* balances the icon's left margin above — without this, a narrow window
     * leaves the last menu ("Help") flush against the right edge while the
     * left side keeps its padding, a visible asymmetry */
    auto *menuRightSpacer = new QWidget(this);
    menuRightSpacer->setFixedWidth(10);
    menuBar()->setCornerWidget(menuRightSpacer, Qt::TopRightCorner);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("connTabs"));
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setExpanding(false); /* SQLyog left-aligns tabs */
    auto *plus = new QPushButton(QStringLiteral("+"), this);
    plus->setFixedSize(22, 20);
    plus->setStyleSheet(QStringLiteral("QPushButton { background: transparent; color: #3B7DBB; "
                                       "border: none; font-weight: bold; }"
                                       "QPushButton:hover { background: #E8F2FA; }"));
    connect(plus, &QPushButton::clicked, this, &MainWindow::newConnection);
    m_tabs->setCornerWidget(plus, Qt::TopRightCorner);

    /* welcome page shown while no connection is open — the empty QTabWidget
     * pane was just a blank rectangle; SQLyog's MDI area is the blue strip.
     * Painted via QPalette, not setStyleSheet(): Qt's stylesheet cascade
     * rule means ANY setStyleSheet() call on an ancestor — even just a
     * "background:" rule — switches every descendant widget without its
     * own stylesheet from native QStyle painting to Qt's own CSS box-model
     * renderer. That silently demoted "New Connection…" below to a flat,
     * slightly-rounded CSS button on every platform (confirmed identical on
     * native Linux and under wine — not a Windows-only rendering gap, see
     * WORKLOG 2026-09-19), which is exactly what a bundled native style
     * plugin (qmodernwindowsstyle.dll) cannot fix: the button had already
     * opted out of native painting before that plugin ever got a say. */
    auto *welcome = new QWidget(this);
    welcome->setAutoFillBackground(true);
    QPalette welcomePal = welcome->palette();
    welcomePal.setColor(QPalette::Window, QColor(0x3B, 0x7D, 0xBB));
    welcome->setPalette(welcomePal);
    auto *wl = new QVBoxLayout(welcome);
    wl->setAlignment(Qt::AlignCenter);
    auto *wIcon = new QLabel(welcome);
    wIcon->setPixmap(QIcon(QStringLiteral(":/resources/openyog-256.png")).pixmap(96, 96));
    wIcon->setAlignment(Qt::AlignCenter);
    auto *wText = new QLabel(QStringLiteral("<div style='color:#EAF2FB;text-align:center'>"
                                            "<h2 style='margin:6px'>OpenYog</h2>"
                                            "No connection open.<br>"
                                            "<b>File → New Connection</b> (Ctrl+M), or the "
                                            "<b>+</b> at the top-right."
                                            "</div>"),
                             welcome);
    wText->setTextFormat(Qt::RichText);
    wText->setAlignment(Qt::AlignCenter);
    auto *wBtn = new QPushButton(QStringLiteral("New Connection…"), welcome);
    wBtn->setFixedWidth(160);
    connect(wBtn, &QPushButton::clicked, this, &MainWindow::newConnection);
    wl->addWidget(wIcon);
    wl->addSpacing(8);
    wl->addWidget(wText);
    wl->addSpacing(12);
    wl->addWidget(wBtn, 0, Qt::AlignCenter);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(welcome);
    m_stack->addWidget(m_tabs);
    setCentralWidget(m_stack);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { syncToolbarToCurrentTab(); });

    /* the menus, in menu-bar order, then the toolbars and status bar
     * (MainWindowMenus.cpp / MainWindowToolsMenus.cpp) */
    buildFileMenu();
    buildEditMenu();
    buildFavoritesMenu();
    buildDatabaseMenu();
    buildTableMenu();
    buildOthersMenu();
    buildToolsMenu();
    buildPowertoolsMenu();
    buildTransactionsMenu();
    buildWindowMenu();
    buildHelpMenu();
    buildToolbarAndStatusBar();
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

int MainWindow::totalLiveConnections() const
{
    int live = 0;
    for(int i = 0; i < m_tabs->count(); ++i)
        if(auto *t = qobject_cast<ConnectionTab *>(m_tabs->widget(i)))
            live += t->liveConnectionCount();
        else if(qobject_cast<MongoTab *>(m_tabs->widget(i)))
            ++live;
    return live;
}

bool MainWindow::openMongo(const ConnectionParams &params, bool interactive)
{
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::shared_ptr<MongoClient> client = MongoClient::connect(params, &error);
    QApplication::restoreOverrideCursor();
    if(!client) {
        const QString where = params.mongoUri.isEmpty()
                                  ? QStringLiteral("%1:%2").arg(params.host).arg(params.port)
                                  : params.mongoUri;
        const QString msg = QStringLiteral("Could not connect to MongoDB at %1.").arg(where);
        if(interactive) {
            QMessageBox::warning(this, QStringLiteral("Connection failed"),
                                 msg + QStringLiteral("\n\n") + error);
        } else {
            statusBar()->showMessage(msg + QLatin1Char(' ') + error, 15000);
            QTextStream(stderr) << msg << ' ' << error << '\n';
        }
        return false;
    }
    ConnectionStore::save(params);
    auto *tab = new MongoTab(params, std::move(client), this);
    const int index = m_tabs->addTab(tab, Icons::get(QStringLiteral("database.ico")), tab->title());
    m_tabs->setCurrentIndex(index);
    connect(tab, &MongoTab::statusMessage, this, [this, tab](const QString &message) {
        if(m_tabs->currentWidget() == tab)
            statusBar()->showMessage(message, 8000);
    });
    syncToolbarToCurrentTab();
    return true;
}

void MainWindow::syncToolbarToCurrentTab()
{
    auto *tab = currentTab();
    m_stack->setCurrentIndex(m_tabs->count() > 0 ? 1 : 0);

    m_dbCombo->blockSignals(true);
    m_dbCombo->clear();
    if(tab) {
        /* schemas of the *current* database only — switching to a
         * different database is the tree's job (click/double-click a
         * database node, or its right-click menu), not this combo's;
         * briefly listing every database here too (session 79) turned
         * out to just duplicate that with a confusing mixed list once
         * switching from the tree already worked, so this stayed scoped
         * to what it always did for MySQL/SQLite: schema selection. */
        m_dbCombo->addItems(tab->databases());
        /* defaultDb(), not currentDatabase(): for Postgres the latter is
         * the *connected* database (e.g. "postgres"), which generally
         * isn't even one of the schema names tab->databases() just
         * populated the combo with — defaultDb() is the schema actually
         * being browsed (see ConnectionTab.h's doc comment on it). Only
         * shown once the user has actually picked one, though — right
         * after connecting (or switching databases), defaultDb() would
         * just be its own "public" fallback, not a real choice, and
         * pre-filling that reads as the combo already having decided for
         * the user rather than being ready for them to.
         *
         * setCurrentText(), not findText()+setCurrentIndex(), for the
         * "true" branch: SQLite's defaultDb() is *always* empty (a SQLite
         * ConnectionParams has no `database`, only a `filePath` — see
         * ConnectionParams.h), which findText() would never match, and
         * explicitly forcing index -1 on that mismatch would blank a
         * SQLite tab's combo unconditionally. setCurrentText() on a
         * non-editable combo just leaves the current (first-item-default)
         * selection alone when nothing matches, so SQLite keeps showing
         * "main" exactly as it always did — only the explicit "not
         * chosen yet" Postgres case above should ever force a blank. */
        if(tab->hasExplicitSchema())
            m_dbCombo->setCurrentText(tab->defaultDb());
        else
            m_dbCombo->setCurrentIndex(-1);
    }
    m_dbCombo->blockSignals(false);

    if(tab) {
        setWindowTitle(QStringLiteral("OpenYog - [%1/%2 - %3]")
                           .arg(tab->title(), tab->defaultDb(), tab->hostLabel()));
        m_driverLabel->setText(tab->driverDisplayLabel());
        m_connectionLabel->setText(tab->activeConnectionLabel());
    } else if(auto *mongo = qobject_cast<MongoTab *>(m_tabs->currentWidget())) {
        setWindowTitle(QStringLiteral("OpenYog - [%1 - MongoDB]").arg(mongo->title()));
        m_driverLabel->setText(QStringLiteral("MongoDB"));
        m_connectionLabel->setText(mongo->title());
    } else {
        setWindowTitle(QStringLiteral("OpenYog"));
        m_driverLabel->setText(QStringLiteral("—"));
        m_connectionLabel->setText(QStringLiteral("No connection"));
    }
    /* counts real server connections (a tab that has browsed another
     * PostgreSQL database holds several at once) — it used to count
     * tabs, which read as a lie the moment a side connection opened */
    m_connectionsLabel->setText(QStringLiteral("Connections: %1").arg(totalLiveConnections()));

    const bool mongo = qobject_cast<MongoTab *>(m_tabs->currentWidget()) != nullptr;
    if(m_mainToolbar && mongo && m_offForMongo.isEmpty()) {
        for(QAction *a : m_mainToolbar->actions())
            if(a != m_newConnAction && !a->isSeparator() && a->isEnabled()) {
                a->setEnabled(false);
                m_offForMongo << a;
            }
    } else if(!mongo && !m_offForMongo.isEmpty()) {
        for(const QPointer<QAction> &a : std::as_const(m_offForMongo))
            if(a)
                a->setEnabled(true);
        m_offForMongo.clear();
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
    if(tab->driverType() == SqlDriverType::Sqlite) {
        /* a SQLite "database" is a file — there's no CREATE DATABASE
         * statement to run against an existing connection at all; a new
         * database means a new connection to a new file */
        QMessageBox::information(this, QStringLiteral("Create Database"),
                                 QStringLiteral("SQLite has no CREATE DATABASE — a database is "
                                                "just a file. Use File → New Connection (Ctrl+M) "
                                                "and pick a new .sqlite file path instead."));
        return;
    }
    const bool pg = tab->driverType() == SqlDriverType::Postgres;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Create Database"),
                                               pg ? QStringLiteral("Schema name:")
                                                  : QStringLiteral("Database name:"),
                                               QLineEdit::Normal, {}, &ok);
    if(!ok || name.isEmpty())
        return;
    /* "database" means schema for Postgres (see PostgresConnection.h) — no
     * per-schema charset to specify there, unlike MySQL's whole-database one */
    if(pg) {
        const QString qname = QLatin1Char('"') +
                              QString(name).replace(QLatin1Char('"'), QStringLiteral("\"\"")) +
                              QLatin1Char('"');
        tab->execDdl(QStringLiteral("CREATE SCHEMA %1").arg(qname));
    } else {
        tab->execDdl(QStringLiteral("CREATE DATABASE `%1` CHARACTER SET utf8mb4").arg(name));
    }
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
    if(what == QStringLiteral("undo"))
        ed->undo();
    else if(what == QStringLiteral("redo"))
        ed->redo();
    else if(what == QStringLiteral("cut"))
        ed->cut();
    else if(what == QStringLiteral("copy"))
        ed->copy();
    else if(what == QStringLiteral("paste"))
        ed->paste();
    else if(what == QStringLiteral("selectall"))
        ed->selectAll();
    else if(what == QStringLiteral("upper") || what == QStringLiteral("lower")) {
        QTextCursor c = ed->textCursor();
        const QString sel = c.selectedText();
        if(!sel.isEmpty())
            c.insertText(what == QStringLiteral("upper") ? sel.toUpper() : sel.toLower());
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

bool MainWindow::openAndRun(const ConnectionParams &params, bool interactive)
{
    /* an SSH host seen for the first time: ask before the tunnel opens
     * (non-interactive, the connect below fails saying the key's untrusted) */
    if(interactive && !ConnectionDialog::confirmSshHostKey(this, params))
        return false;
    if(params.driverType == SqlDriverType::Mongo)
        return openMongo(params, interactive);
    auto *tab = new ConnectionTab(params, this);
    if(!tab->isConnected()) {
        /* with the reason: a wrong password, an untrusted SSH host key and a
         * server that's down otherwise all read the same */
        const QString msg = QStringLiteral("Could not connect to %1@%2:%3.")
                                .arg(params.user, params.host)
                                .arg(params.port);
        const QString why = tab->connectError().trimmed();
        if(interactive) {
            QMessageBox::warning(this, QStringLiteral("Connection failed"),
                                 why.isEmpty() ? msg : msg + QStringLiteral("\n\n") + why);
        } else {
            statusBar()->showMessage(why.isEmpty() ? msg : msg + QLatin1Char(' ') + why, 15000);
            /* straight to stderr: qWarning() goes to the journal whenever
             * stderr isn't a terminal, i.e. in every scripted selftest run */
            QTextStream(stderr) << msg << ' ' << why << '\n';
        }
        delete tab;
        return false;
    }

    ConnectionStore::save(params);
    const int index = m_tabs->addTab(tab, tab->title());
    m_tabs->setCurrentIndex(index);
    tab->restoreEditors(SessionStore::editors(params.name));

    /* stash connection info for "New Connection Using Current Settings" —
     * driverType/filePath included so that action actually reconnects with
     * the right driver instead of always assuming MySQL */
    tab->setProperty("host", params.host);
    tab->setProperty("port", params.port);
    tab->setProperty("user", params.user);
    tab->setProperty("password", params.password);
    tab->setProperty("database", params.database);
    tab->setProperty("driverType", static_cast<int>(params.driverType));
    tab->setProperty("filePath", params.filePath);

    connect(tab, &ConnectionTab::databasesChanged, this,
            [this, tab](const QStringList &, const QString &) {
                /* also fires from switchDatabase() (PostgreSQL "Switch to `db`") —
                 * the tab's own name/title changed along with it, but its entry
                 * in the tab bar was only ever set once, at addTab() time below */
                const int idx = m_tabs->indexOf(tab);
                if(idx >= 0)
                    m_tabs->setTabText(idx, tab->title());
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
    connect(tab, &ConnectionTab::cursorMoved, this, [this, tab](const QString &pos) {
        if(m_tabs->currentWidget() == tab)
            m_cursorLabel->setText(pos);
    });
    connect(tab, &ConnectionTab::activeConnectionChanged, this, [this, tab](const QString &label) {
        if(m_tabs->currentWidget() != tab)
            return;
        m_connectionLabel->setText(label);
        /* every emission coincides with the live-connection count possibly
         * changing (a side connection opened, switchDatabase swapped the
         * primary) — so re-count here too, not just on tab switches */
        m_connectionsLabel->setText(QStringLiteral("Connections: %1").arg(totalLiveConnections()));
    });
    connect(tab, &ConnectionTab::newTabRequested, this,
            [this](const ConnectionParams &p) { openAndRun(p); });

    syncToolbarToCurrentTab();
    return true;
}

/* Table ▸ Copy Table(s) To Different Host/Database… — the same body the
 * menu action runs, kept public so the --copytablehost selftest can
 * exercise the non-MySQL-source guard headlessly */
void MainWindow::copySelectedTableToHost()
{
    auto *t = currentTab();
    if(!t)
        return;
    const QStringList info = t->selectedTableInfo();
    if(info.size() < 2) {
        QMessageBox::information(this, QStringLiteral("OpenYog"),
                                 QStringLiteral("Select a table in the object browser first."));
        return;
    }
    t->promptCopyTableToHost(info[0], info[1]);
}

void MainWindow::closeTab(int index)
{
    QWidget *w = m_tabs->widget(index);
    if(auto *t = qobject_cast<ConnectionTab *>(w))
        SessionStore::setEditors(t->params().name, t->editorStates());
    m_tabs->removeTab(index);
    delete w;
    syncToolbarToCurrentTab();
}

void MainWindow::saveSessionState()
{
    QStringList open;
    for(int i = 0; i < m_tabs->count(); ++i)
        if(auto *t = qobject_cast<ConnectionTab *>(m_tabs->widget(i))) {
            SessionStore::setEditors(t->params().name, t->editorStates());
            open << t->params().name;
        } else if(auto *m = qobject_cast<MongoTab *>(m_tabs->widget(i))) {
            open << m->params().name;
        }
    open.removeDuplicates();
    SessionStore::setOpenConnections(open);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSessionState();
    QMainWindow::closeEvent(event);
}

void MainWindow::restoreLastSession()
{
    if(!SessionStore::restoreOnStartup())
        return;
    for(const QString &name : SessionStore::openConnections()) {
        ConnectionParams p;
        if(!ConnectionStore::load(name, &p))
            continue; /* deleted since */
        statusBar()->showMessage(QStringLiteral("Reconnecting to %1…").arg(name));
        QApplication::processEvents(); /* show that before the connect blocks */
        openAndRun(p, false);
    }
}

void MainWindow::openSchemaSync()
{
    /* every connected tab, the current one first (the default source) */
    QList<ConnectionTab *> tabs;
    if(ConnectionTab *current = currentTab(); current && current->isConnected())
        tabs << current;
    for(int i = 0; i < m_tabs->count(); ++i)
        if(auto *t = qobject_cast<ConnectionTab *>(m_tabs->widget(i));
           t && t->isConnected() && !tabs.contains(t))
            tabs << t;
    if(tabs.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Schema Synchronization"),
                                 QStringLiteral("Open a connection first."));
        return;
    }
    QList<SchemaSyncDialog::Endpoint> endpoints;
    for(ConnectionTab *t : std::as_const(tabs)) {
        /* the dialog can outlive a tab closed behind it */
        QPointer<ConnectionTab> tab(t);
        SchemaSyncDialog::Endpoint e;
        e.label = t->title();
        e.driver = t->driverType();
        e.databases = [tab] { return tab ? tab->databases() : QStringList(); };
        e.currentDatabase = [tab] { return tab ? tab->defaultDb() : QString(); };
        e.load = [tab](const QString &db) {
            return tab ? tab->syncSchema(db) : SchemaSync::Schema();
        };
        e.openInEditor = [this, tab](const QString &sql) {
            if(!tab)
                return;
            m_tabs->setCurrentWidget(tab);
            tab->openEditorWithSql(QStringLiteral("Schema Sync"), sql);
            activateWindow();
        };
        endpoints << e;
    }
    auto *dialog = new SchemaSyncDialog(endpoints, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::openDataSync()
{
    /* every connected tab, the current one first (the default source) */
    QList<ConnectionTab *> tabs;
    if(ConnectionTab *current = currentTab(); current && current->isConnected())
        tabs << current;
    for(int i = 0; i < m_tabs->count(); ++i)
        if(auto *t = qobject_cast<ConnectionTab *>(m_tabs->widget(i));
           t && t->isConnected() && !tabs.contains(t))
            tabs << t;
    if(tabs.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Data Synchronization"),
                                 QStringLiteral("Open a connection first."));
        return;
    }
    QList<DataSyncDialog::Endpoint> endpoints;
    for(ConnectionTab *t : std::as_const(tabs)) {
        QPointer<ConnectionTab> tab(t);
        DataSyncDialog::Endpoint e;
        e.label = t->title();
        e.driver = t->driverType();
        e.databases = [tab] { return tab ? tab->databases() : QStringList(); };
        e.currentDatabase = [tab] { return tab ? tab->defaultDb() : QString(); };
        /* the tab's own connection settings: every statement names its
         * database (MySQL) or schema (PostgreSQL) itself */
        e.params = [params = t->params()](const QString &) { return params; };
        e.openInEditor = [this, tab](const QString &sql) {
            if(!tab)
                return;
            m_tabs->setCurrentWidget(tab);
            tab->openEditorWithSql(QStringLiteral("Data Sync"), sql);
            activateWindow();
        };
        endpoints << e;
    }
    auto *dialog = new DataSyncDialog(endpoints, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}
