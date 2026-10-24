/* OpenYog — main window: the Tools, Powertools, Transactions, Window and Help
 * menus, then the toolbars and the status bar.
 * The class is declared in MainWindow.h; MainWindow.cpp holds the core.
 * The menus are transcribed from the upstream resource script
 * (include/SQLyog.rc, IDR_MAINMENU): labels, nesting and shortcuts are
 * verbatim. Items whose functionality hasn't been ported yet stay visible but
 * disabled — they mark the roadmap (FEATURES.md); working items are wired. */
#include "MainWindow.h"
#include "ConnectionDialog.h"
#include "ConnectionTab.h"
#include "Icons.h"
#include "ObjectBrowser.h"
#include "SessionStore.h"
#include "Theme.h"

#include <QAction>
#include <QApplication>
#include <QActionGroup>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QPixmap>
#include <QFileDialog>
#include <QFile>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QMessageBox>
#include <QPushButton>
#include <QFrame>
#include <QKeySequence>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include <functional>

void MainWindow::buildToolsMenu()
{
    QAction *openDataOnDbl = m_shared.openDataOnDbl;
    /* ================= Tools ======================================== */
    QMenu *tools = menuBar()->addMenu(QStringLiteral("&Tools"));
    QAction *exportRows = m_shared.exportRows =
        tools->addAction(QStringLiteral("&Export All Rows Of Table Data/Result As…\tCtrl+Shift+E"));
    connect(exportRows, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->exportCurrent();
    });
    QAction *toolsDump =
        tools->addAction(QStringLiteral("&Backup Database As SQL Dump…\tCtrl+Alt+E"));
    toolsDump->setIcon(Icons::get(QStringLiteral("export_data_16.ico")));
    connect(toolsDump, &QAction::triggered, this, [this] { dumpDatabase(); });
    QAction *runScript = m_shared.runScript =
        tools->addAction(QStringLiteral("Execute &SQL Script…\tCtrl+Shift+Q"));
    connect(runScript, &QAction::triggered, this, [this] {
        if(auto *t = currentTab()) {
            const QString f =
                QFileDialog::getOpenFileName(this, QStringLiteral("Execute SQL script"), QString(),
                                             QStringLiteral("SQL (*.sql);;All (*)"));
            if(!f.isEmpty())
                t->openSqlFile(f); /* loads, so the user sees what runs */
        }
    });
    tools->addSeparator();
    QMenu *flushMenu = tools->addMenu(QStringLiteral("&Flush…\tCtrl+Alt+F"));
    /* upstream's Flush is a single command (ID_TOOLS_FLUSH) that pops its own
     * floating menu rather than a nested submenu — the icon still belongs on
     * that one launcher action, which here is the QMenu's own menuAction() */
    flushMenu->menuAction()->setIcon(Icons::get(QStringLiteral("flush.ico")));
    const auto flushWith = [this](const QString &sql) {
        auto *t = currentTab();
        if(!t)
            return;
        if(t->driverType() != SqlDriverType::Mysql) {
            QMessageBox::information(this, QStringLiteral("Flush"),
                                     QStringLiteral("FLUSH is MySQL-only."));
            return;
        }
        t->runStatements(QStringList{sql}, QStringLiteral("Flush"));
    };
    for(const auto &[label, sql] :
        {std::pair{QStringLiteral("&Tables"), QStringLiteral("FLUSH TABLES")},
         std::pair{QStringLiteral("&Privileges"), QStringLiteral("FLUSH PRIVILEGES")},
         std::pair{QStringLiteral("&Logs"), QStringLiteral("FLUSH LOGS")},
         std::pair{QStringLiteral("&Hosts"), QStringLiteral("FLUSH HOSTS")},
         std::pair{QStringLiteral("&Status"), QStringLiteral("FLUSH STATUS")}}) {
        QAction *a = flushMenu->addAction(label);
        connect(a, &QAction::triggered, this, [flushWith, sql] { flushWith(sql); });
    }
    QAction *diag = tools->addAction(QStringLiteral("&Table Diagnostics…\tCtrl+Alt+T"));
    diag->setIcon(Icons::get(QStringLiteral("tablediag.ico")));
    connect(diag, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::tableDiagnostics); });
    QAction *history = tools->addAction(QStringLiteral("&History\tCtrl+Shift+H"));
    history->setIcon(Icons::get(QStringLiteral("history.ico")));
    connect(history, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->showHistory();
    });
    QAction *info = tools->addAction(QStringLiteral("&Info\tCtrl+Shift+I"));
    info->setIcon(Icons::get(QStringLiteral("object.ico")));
    connect(info, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->showConnectionInfo();
        else
            QMessageBox::information(this, QStringLiteral("Info"),
                                     QStringLiteral("Open a connection first."));
    });
    tools->addSeparator();
    QAction *userMgr = tools->addAction(QStringLiteral("&User Manager\tCtrl+U"));
    userMgr->setIcon(Icons::get(QStringLiteral("user16.ico")));
    userMgr->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
    connect(userMgr, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->promptUserManager();
        else
            QMessageBox::information(this, QStringLiteral("User Manager"),
                                     QStringLiteral("Open a connection first."));
    });
    /* every top-level Tools item carries an icon (owner directive);
     * icons follow upstream's id→bitmap map where one exists
     * (ConnectionCommunity::CreateIconList: Export All→IDI_XMLHTML_16,
     * Backup→IDI_EXPORTDATA_16, …) and pick a bundled bitmap for the
     * port-only / upstream-iconless rows. Submenu children stay plain —
     * upstream's Show/Theme/language children have no icons either */
    QMenu *show = tools->addMenu(QStringLiteral("Sho&w"));
    show->menuAction()->setIcon(Icons::get(QStringLiteral("view.ico")));
    /* SQLite has none of these concepts at all (no server, no runtime
     * parameters, no other connections to list) — guarded with a plain
     * message; PostgreSQL has real equivalents, just different statements,
     * not MySQL's SHOW syntax at all. */
    const auto showFor = [this](const QString &mysqlSql, const QString &pgSql,
                                const QString &tabTitle) {
        auto *t = currentTab();
        if(!t)
            return;
        if(t->driverType() == SqlDriverType::Sqlite) {
            QMessageBox::information(this, tabTitle,
                                     QStringLiteral("SQLite has no server to show this for."));
            return;
        }
        t->runStatements(QStringList{t->driverType() == SqlDriverType::Postgres ? pgSql : mysqlSql},
                         tabTitle);
    };
    QAction *showVars = show->addAction(QStringLiteral("&Variables…"));
    connect(showVars, &QAction::triggered, this, [showFor] {
        showFor(QStringLiteral("SHOW VARIABLES"), QStringLiteral("SHOW ALL"),
                QStringLiteral("Variables"));
    });
    QAction *showProc = show->addAction(QStringLiteral("&Processlist…"));
    connect(showProc, &QAction::triggered, this, [showFor] {
        showFor(QStringLiteral("SHOW FULL PROCESSLIST"),
                QStringLiteral("SELECT * FROM pg_stat_activity"), QStringLiteral("Processlist"));
    });
    QAction *showStatus = show->addAction(QStringLiteral("&Status…"));
    connect(showStatus, &QAction::triggered, this, [showFor] {
        showFor(QStringLiteral("SHOW STATUS"), QStringLiteral("SELECT * FROM pg_stat_database"),
                QStringLiteral("Status"));
    });
    tools->addSeparator();
    QAction *changeLang = addDisabled(tools, QStringLiteral("Change &Language\tAlt+Shift+L"));
    changeLang->setIcon(Icons::get(QStringLiteral("user-globe.ico")));
    QMenu *connDetails = tools->addMenu(QStringLiteral("Export/I&mport Connection Details"));
    connDetails->menuAction()->setIcon(Icons::get(QStringLiteral("export_result.ico")));
    QAction *exportConn = connDetails->addAction(QStringLiteral("&Export Connection Details…"));
    connect(exportConn, &QAction::triggered, this, [this] {
        auto *t = currentTab();
        if(!t) {
            QMessageBox::information(this, QStringLiteral("Export Connection Details"),
                                     QStringLiteral("Open a connection first."));
            return;
        }
        const ConnectionParams &p = t->params();
        const QString file = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export Connection Details"), p.name + QStringLiteral(".json"),
            QStringLiteral("Connection details (*.json)"));
        if(file.isEmpty())
            return;
        QJsonObject o;
        o["name"] = p.name;
        o["driver"] = driverTypeToString(p.driverType);
        o["host"] = p.host;
        o["port"] = p.port;
        o["user"] = p.user;
        o["database"] = p.database;
        o["filepath"] = p.filePath;
        /* password deliberately left out — this file is meant to be
         * shareable/backed-up; re-enter it on import instead */
        QFile f(file);
        if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, QStringLiteral("Export Connection Details"),
                                 QStringLiteral("Could not write %1").arg(file));
            return;
        }
        f.write(QJsonDocument(o).toJson());
        QMessageBox::information(this, QStringLiteral("Export Connection Details"),
                                 QStringLiteral("Saved to %1\n(password not included — "
                                                "you'll be asked for it again on import)")
                                     .arg(file));
    });
    QAction *importConn = connDetails->addAction(QStringLiteral("&Import Connection Details…"));
    connect(importConn, &QAction::triggered, this, [this] {
        const QString file =
            QFileDialog::getOpenFileName(this, QStringLiteral("Import Connection Details"),
                                         QString(), QStringLiteral("Connection details (*.json)"));
        if(file.isEmpty())
            return;
        QFile f(file);
        if(!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, QStringLiteral("Import Connection Details"),
                                 QStringLiteral("Could not read %1").arg(file));
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        ConnectionParams p;
        p.name = o.value("name").toString(QStringLiteral("Imported connection"));
        p.driverType = driverTypeFromString(o.value("driver").toString());
        p.host = o.value("host").toString(p.host);
        p.port = o.value("port").toInt(p.port);
        p.user = o.value("user").toString();
        p.database = o.value("database").toString();
        p.filePath = o.value("filepath").toString();
        if(p.driverType == SqlDriverType::Mysql) {
            bool ok = false;
            p.password = QInputDialog::getText(
                this, QStringLiteral("Import Connection Details"),
                QStringLiteral("Password for %1@%2 (left blank if none):").arg(p.user, p.host),
                QLineEdit::Password, QString(), &ok);
            if(!ok)
                return;
        }
        openAndRun(p);
    });
    QAction *preferences = tools->addAction(QStringLiteral("&Preferences…"));
    preferences->setIcon(Icons::get(QStringLiteral("preferences.ico")));
    connect(preferences, &QAction::triggered, this, [this, openDataOnDbl] {
        /* one dialog over the same settings the Theme submenu, Query
         * Timeout… action and Change Object Browser Color already expose
         * individually — a single stop for the handful of app-wide (not
         * per-connection) settings that exist so far. */
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("Preferences"));
        auto *themeCombo = new QComboBox(&dlg);
        themeCombo->addItems(
            {QStringLiteral("light"), QStringLiteral("dark"), QStringLiteral("twilight")});
        themeCombo->setCurrentText(Theme::load());
        auto *timeoutSpin = new QSpinBox(&dlg);
        timeoutSpin->setRange(0, 24 * 3600);
        timeoutSpin->setValue(ConnectionTab::queryTimeoutSecs());
        timeoutSpin->setSpecialValueText(QStringLiteral("never"));
        timeoutSpin->setLocale(QLocale::c());
        auto *rowsSpin = new QSpinBox(&dlg);
        rowsSpin->setRange(0, 100000000);
        rowsSpin->setSingleStep(1000);
        rowsSpin->setValue(ConnectionTab::resultRowLimit());
        rowsSpin->setSpecialValueText(QStringLiteral("all"));
        rowsSpin->setToolTip(QStringLiteral("A query's result shows its first rows only; the rest "
                                            "are counted, not kept, so a huge result can't fill "
                                            "the memory"));
        QColor browserColor = ObjectBrowserColor::load();
        /* upstream's Preferences checkbox (SQLyog.rc "Paste object name in
         * &editor on double-click", key GetTextOnDBClick) — checked keeps
         * the name-into-editor gesture, unchecked makes a table's
         * double-click open its Table Data (same key the Edit-menu toggle
         * and the tree's own handler use) */
        auto *dblClickBox =
            new QCheckBox(QStringLiteral("Paste object name in editor on double-click"), &dlg);
        dblClickBox->setChecked(ObjectBrowserDoubleClick::pasteNameInEditor());
        auto *restoreBox = new QCheckBox(
            QStringLiteral("Reopen last session's connections and query tabs on startup"), &dlg);
        restoreBox->setChecked(SessionStore::restoreOnStartup());
        auto *colorBtn = new QPushButton(
            browserColor.isValid() ? browserColor.name() : QStringLiteral("(theme default)"), &dlg);
        connect(colorBtn, &QPushButton::clicked, &dlg, [&] {
            const QColor c =
                QColorDialog::getColor(browserColor.isValid() ? browserColor : QColor(Qt::white),
                                       &dlg, QStringLiteral("Object Browser Selection Color"));
            if(c.isValid()) {
                browserColor = c;
                colorBtn->setText(c.name());
            }
        });
        auto *form = new QFormLayout;
        form->addRow(QStringLiteral("Theme"), themeCombo);
        form->addRow(QStringLiteral("Query timeout (seconds)"), timeoutSpin);
        form->addRow(QStringLiteral("Rows kept from a result"), rowsSpin);
        form->addRow(QStringLiteral("Object browser selection color"), colorBtn);
        form->addRow(QString(), dblClickBox);
        form->addRow(QString(), restoreBox);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        auto *lay = new QVBoxLayout(&dlg);
        lay->addLayout(form);
        lay->addWidget(buttons);
        if(dlg.exec() != QDialog::Accepted)
            return;

        const QString theme = themeCombo->currentText();
        Theme::save(theme);
        Theme::apply(*qApp, theme);
        ConnectionTab::setQueryTimeoutSecs(timeoutSpin->value());
        ConnectionTab::setResultRowLimit(rowsSpin->value());
        ObjectBrowserColor::save(browserColor);
        ObjectBrowserDoubleClick::setPasteNameInEditor(dblClickBox->isChecked());
        SessionStore::setRestoreOnStartup(restoreBox->isChecked());
        openDataOnDbl->setChecked(!dblClickBox->isChecked());
    });
    QAction *queryTimeout = tools->addAction(QStringLiteral("Query &Timeout…"));
    queryTimeout->setIcon(Icons::get(QStringLiteral("schdexport_16.ico")));
    connect(queryTimeout, &QAction::triggered, this, [this] {
        bool ok = false;
        const int cur = ConnectionTab::queryTimeoutSecs();
        const int secs =
            QInputDialog::getInt(this, QStringLiteral("Query Timeout"),
                                 QStringLiteral("Cancel a running query after this many seconds\n"
                                                "(0 = never):"),
                                 cur, 0, 24 * 3600, 1, &ok);
        if(ok)
            ConnectionTab::setQueryTimeoutSecs(secs);
    });
    QMenu *themeMenu = tools->addMenu(QStringLiteral("&Theme"));
    themeMenu->menuAction()->setIcon(Icons::get(QStringLiteral("colorpicker.ico")));
    auto *themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    const QString curTheme = Theme::load();
    for(const auto &[label, id] : {QPair{QStringLiteral("&Light"), QStringLiteral("light")},
                                   QPair{QStringLiteral("&Dark"), QStringLiteral("dark")},
                                   QPair{QStringLiteral("Twilight"), QStringLiteral("twilight")}}) {
        QAction *a = themeMenu->addAction(label);
        a->setCheckable(true);
        a->setChecked(id == curTheme);
        themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [id] {
            Theme::save(id);
            Theme::apply(*qApp, id);
        });
    }
}

void MainWindow::buildPowertoolsMenu()
{
    /* ================= Powertools =================================== */
    QMenu *powertools = menuBar()->addMenu(QStringLiteral("&Powertools"));
    QAction *dataSync =
        powertools->addAction(QStringLiteral("Database S&ynchronization Wizard…\tCtrl+Alt+W"));
    dataSync->setIcon(Icons::get(QStringLiteral("dbsync_16.ico")));
    connect(dataSync, &QAction::triggered, this, &MainWindow::openDataSync);
    addDisabled(powertools, QStringLiteral("&Visual Data Comparison Wizard…\tCtrl+Alt+Q"))
        ->setIcon(Icons::get(QStringLiteral("Sync_16.ico")));
    QAction *schemaSync =
        powertools->addAction(QStringLiteral("Schema &Synchronization Tool…\tCtrl+Q"));
    schemaSync->setIcon(Icons::get(QStringLiteral("schema.ico")));
    connect(schemaSync, &QAction::triggered, this, &MainWindow::openSchemaSync);
    powertools->addSeparator();
    powertools->addAction(importExternalAction());
    addDisabled(powertools, QStringLiteral("S&QL Scheduler and Reporting Wizard…\tCtrl+Alt+N"))
        ->setIcon(Icons::get(QStringLiteral("notification_16.ico")));
    addDisabled(powertools, QStringLiteral("S&cheduled Backups…\tCtrl+Alt+S"))
        ->setIcon(Icons::get(QStringLiteral("schdexport.ico")));
    powertools->addSeparator();
    addDisabled(powertools, QStringLiteral("Scheduled &Jobs…"))
        ->setIcon(Icons::get(QStringLiteral("jobmanager_16.ico")));
    powertools->addSeparator();
    QAction *rebuildTags = powertools->addAction(QStringLiteral("&Rebuild tags"));
    rebuildTags->setIcon(Icons::get(QStringLiteral("entmenu.ico")));
    connect(rebuildTags, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->refreshBrowser(); /* re-syncs the object tree + autocomplete tags */
    });
}

/* one transaction statement on the current tab's connection */
void MainWindow::runTx(const QString &sql)
{
    if(auto *t = currentTab())
        t->runStatements(QStringList{sql}, QStringLiteral("Transaction"));
}

void MainWindow::buildTransactionsMenu()
{
    /* ================= Transactions ================================= */
    QMenu *transactions = menuBar()->addMenu(QStringLiteral("T&ransactions"));
    /* Set Autocommit and Commit RELEASE/NO RELEASE are genuinely MySQL-only
     * (no SQL-level autocommit toggle in Postgres at all — psql's own
     * \set AUTOCOMMIT is a client setting, not something SET can touch; and
     * RELEASE, which closes the client connection after commit, has no
     * Postgres or SQLite equivalent either). Guarded the same way Flush
     * guards itself. */
    const auto mysqlOnlyTx = [this](const QString &sql) {
        auto *t = currentTab();
        if(!t)
            return;
        if(t->driverType() != SqlDriverType::Mysql) {
            QMessageBox::information(this, QStringLiteral("Transactions"),
                                     QStringLiteral("MySQL only."));
            return;
        }
        t->runStatements(QStringList{sql}, QStringLiteral("Transaction"));
    };
    /* isolation level and chained commit ARE standard SQL PostgreSQL
     * supports directly (confirmed against a live server: "SET SESSION
     * TRANSACTION ISOLATION LEVEL x" and "COMMIT AND [NO] CHAIN" both work
     * as-is, the identical MySQL phrasing) — only SQLite has neither
     * concept (its own isolation is fixed by its locking mode, not
     * settable per-session, and it has no chained-commit syntax). */
    const auto notSqliteTx = [this](const QString &sql) {
        auto *t = currentTab();
        if(!t)
            return;
        if(t->driverType() == SqlDriverType::Sqlite) {
            QMessageBox::information(this, QStringLiteral("Transactions"),
                                     QStringLiteral("Not supported on SQLite."));
            return;
        }
        t->runStatements(QStringList{sql}, QStringLiteral("Transaction"));
    };

    QAction *autocommit = transactions->addAction(QStringLiteral("Set Autocommit"));
    autocommit->setCheckable(true);
    autocommit->setCheckable(true);
    autocommit->setChecked(true);
    connect(autocommit, &QAction::toggled, this, [mysqlOnlyTx](bool on) {
        mysqlOnlyTx(QStringLiteral("SET autocommit=%1").arg(on ? 1 : 0));
    });

    QMenu *isolation = transactions->addMenu(QStringLiteral("Isolation Level"));
    isolation->menuAction()->setIcon(Icons::get(QStringLiteral("entmenu.ico")));
    auto *isoGroup = new QActionGroup(this);
    isoGroup->setExclusive(true);
    for(const auto &[label, level] :
        {std::pair{QStringLiteral("Repeatable Read"), QStringLiteral("REPEATABLE READ")},
         std::pair{QStringLiteral("Read Committed"), QStringLiteral("READ COMMITTED")},
         std::pair{QStringLiteral("Read Uncommitted"), QStringLiteral("READ UNCOMMITTED")},
         std::pair{QStringLiteral("Serializable"), QStringLiteral("SERIALIZABLE")}}) {
        QAction *a = isolation->addAction(label);
        a->setCheckable(true);
        isoGroup->addAction(a);
        connect(a, &QAction::triggered, this, [notSqliteTx, level] {
            notSqliteTx(QStringLiteral("SET SESSION TRANSACTION ISOLATION LEVEL %1").arg(level));
        });
    }
    transactions->addSeparator();

    QMenu *startTrx = transactions->addMenu(QStringLiteral("Start Transaction"));
    startTrx->menuAction()->setIcon(Icons::get(QStringLiteral("start_transaction_16.ico")));
    QAction *startPlain = startTrx->addAction(QStringLiteral("With no modifier"));
    connect(startPlain, &QAction::triggered, this, [this] {
        auto *t = currentTab();
        if(!t)
            return;
        runTx(t->driverType() == SqlDriverType::Sqlite ? QStringLiteral("BEGIN")
                                                       : QStringLiteral("START TRANSACTION"));
    });
    /* "WITH CONSISTENT SNAPSHOT" is MySQL/InnoDB-specific phrasing for a
     * repeatable-read snapshot semantic Postgres reaches differently — but
     * the READ ONLY/READ WRITE transaction mode underneath is standard SQL
     * Postgres supports directly (confirmed against a live server), just
     * without that clause. SQLite has no transaction-mode concept at all. */
    QMenu *snapshot = startTrx->addMenu(QStringLiteral("With Consistent Snapshot"));
    QAction *snapRO = snapshot->addAction(QStringLiteral("Read only"));
    connect(snapRO, &QAction::triggered, this, [this, notSqliteTx] {
        auto *t = currentTab();
        if(!t)
            return;
        notSqliteTx(t->driverType() == SqlDriverType::Postgres
                        ? QStringLiteral("START TRANSACTION READ ONLY")
                        : QStringLiteral("START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY"));
    });
    QAction *snapRW = snapshot->addAction(QStringLiteral("Read Write"));
    connect(snapRW, &QAction::triggered, this, [this, notSqliteTx] {
        auto *t = currentTab();
        if(!t)
            return;
        notSqliteTx(t->driverType() == SqlDriverType::Postgres
                        ? QStringLiteral("START TRANSACTION READ WRITE")
                        : QStringLiteral("START TRANSACTION WITH CONSISTENT SNAPSHOT, READ WRITE"));
    });

    QMenu *commit = transactions->addMenu(QStringLiteral("Commit"));
    commit->menuAction()->setIcon(Icons::get(QStringLiteral("commit_16.ico")));
    QAction *commitPlain = commit->addAction(QStringLiteral("With no modifier"));
    connect(commitPlain, &QAction::triggered, this, [this] { runTx(QStringLiteral("COMMIT")); });
    commit->addSeparator();
    QAction *commitChain = commit->addAction(QStringLiteral("And Chain"));
    connect(commitChain, &QAction::triggered, this,
            [notSqliteTx] { notSqliteTx(QStringLiteral("COMMIT AND CHAIN")); });
    QAction *commitNoChain = commit->addAction(QStringLiteral("And No Chain"));
    connect(commitNoChain, &QAction::triggered, this,
            [notSqliteTx] { notSqliteTx(QStringLiteral("COMMIT AND NO CHAIN")); });
    commit->addSeparator();
    QAction *commitRelease = commit->addAction(QStringLiteral("Release"));
    connect(commitRelease, &QAction::triggered, this,
            [mysqlOnlyTx] { mysqlOnlyTx(QStringLiteral("COMMIT RELEASE")); });
    QAction *commitNoRelease = commit->addAction(QStringLiteral("No Release"));
    connect(commitNoRelease, &QAction::triggered, this,
            [mysqlOnlyTx] { mysqlOnlyTx(QStringLiteral("COMMIT NO RELEASE")); });

    QMenu *rollback = transactions->addMenu(QStringLiteral("Rollback"));
    rollback->menuAction()->setIcon(Icons::get(QStringLiteral("rollback_16.ico")));
    QAction *rollToSave = rollback->addAction(QStringLiteral("To Savepoint"));
    connect(rollToSave, &QAction::triggered, this, [this] {
        auto *t = currentTab();
        if(!t)
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Rollback To Savepoint"),
                                                   QStringLiteral("Savepoint name:"),
                                                   QLineEdit::Normal, QString(), &ok);
        if(ok && !name.trimmed().isEmpty())
            t->runStatements(
                QStringList{QStringLiteral("ROLLBACK TO SAVEPOINT %1").arg(name.trimmed())},
                QStringLiteral("Transaction"));
    });
    QAction *rollTrx = rollback->addAction(QStringLiteral("Transaction"));
    connect(rollTrx, &QAction::triggered, this, [this] { runTx(QStringLiteral("ROLLBACK")); });
}

void MainWindow::buildWindowMenu()
{
    /* ================= Window ======================================= */
    QMenu *window = menuBar()->addMenu(QStringLiteral("&Window"));
    QAction *winClose = window->addAction(QStringLiteral("&Close Tab\tAlt+L"));
    winClose->setIcon(Icons::get(QStringLiteral("closetab.ico")));
    connect(winClose, &QAction::triggered, this, [this] {
        if(m_tabs->currentIndex() >= 0)
            closeTab(m_tabs->currentIndex());
    });
    QAction *winNext = window->addAction(QStringLiteral("&Next Tab\tCtrl+PgDown"));
    winNext->setIcon(Icons::get(QStringLiteral("Down.ico")));
    connect(winNext, &QAction::triggered, this, [this] { switchTab(1); });
    QAction *winPrev = window->addAction(QStringLiteral("&Previous Tab\tCtrl+PgUp"));
    winPrev->setIcon(Icons::get(QStringLiteral("Up.ico")));
    connect(winPrev, &QAction::triggered, this, [this] { switchTab(-1); });
}

void MainWindow::buildHelpMenu()
{
    /* ================= Help ========================================= */
    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction *about = help->addAction(QStringLiteral("&About OpenYog"));
    about->setIcon(QIcon(QPixmap(QStringLiteral(":/resources/openyog-16.png"))));
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::information(
            this, QStringLiteral("About OpenYog"),
            QStringLiteral("OpenYog %1 — a cross-platform MySQL/MariaDB, PostgreSQL and SQLite "
                           "client.\n"
                           "GPL-3.0 fork of the SQLyog Community source.\n"
                           "Not affiliated with Webyog/Idera.")
                .arg(QCoreApplication::applicationVersion()));
    });
}

void MainWindow::buildToolbarAndStatusBar()
{
    QAction *newConn = m_shared.newConn;
    QAction *newEditor = m_shared.newEditor;
    QAction *manageIdx = m_shared.manageIdx;
    QAction *manageFk = m_shared.manageFk;
    QAction *copyDb = m_shared.copyDb;
    QAction *exportRows = m_shared.exportRows;
    QAction *runScript = m_shared.runScript;
    QAction *expTblData = m_shared.expTblData;
    /* ================= toolbar + status bar ========================= */
    /* button order + icons follow SQLyog's two Win32 toolbars: the main
     * one (FrameWindow::CreateToolButtons — connect, new editor, execute,
     * execute-all, execute&edit, refresh) and the second one laid out to
     * its right (ConnectionBase::CreateOtherToolButtons — user manager,
     * export, execute script, copy database, export-as, manage indexes,
     * manage relationships, format current, start transaction, commit,
     * rollback). Upstream also puts Data Sync, Diff Tool, ODBC import,
     * Notification, Scheduled Backup, Query Builder and Schema Designer
     * buttons on that second toolbar — none of those features exist in
     * this port yet, so their buttons come with the features. Where the
     * action already exists as a menu item the menu's QAction is reused
     * (one action = one shortcut, one enable-state, icon in both places). */
    auto *toolbar = addToolBar(QStringLiteral("main"));
    toolbar->setMovable(false);
    m_mainToolbar = toolbar;
    m_newConnAction = newConn;
    constexpr int kToolbarIcon = 18;
    toolbar->setIconSize(QSize(kToolbarIcon, kToolbarIcon));

    newConn->setIcon(Icons::get(QStringLiteral("connect_16.ico")));
    toolbar->addAction(newConn);
    newEditor->setIcon(Icons::get(QStringLiteral("query_16.ico")));
    toolbar->addAction(newEditor);
    toolbar->addSeparator();

    QAction *execTool = toolbar->addAction(Icons::get(QStringLiteral("execute_16.ico")),
                                           QStringLiteral("Execute Query\tF9"));
    connect(execTool, &QAction::triggered, this, &MainWindow::executeCurrentTab);
    QAction *execAllTool = toolbar->addAction(Icons::get(QStringLiteral("execall_16.ico")),
                                              QStringLiteral("Execute All Queries\tCtrl+F9"));
    connect(execAllTool, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->runAll();
    });
    QAction *execEditTool =
        toolbar->addAction(Icons::get(QStringLiteral("execforupd_16.ico")),
                           QStringLiteral("Execute Query & Edit Resultset\tF8"));
    connect(execEditTool, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->runAndEdit();
    });
    QAction *stopTool = toolbar->addAction(Icons::get(QStringLiteral("Stop_16.ico")),
                                           QStringLiteral("Cancel the running query"));
    connect(stopTool, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->cancelQuery();
    });
    toolbar->addSeparator();

    QAction *refreshTool = toolbar->addAction(Icons::get(QStringLiteral("refresh_16.ico")),
                                              QStringLiteral("Refresh Object Browser\tF5"));
    connect(refreshTool, &QAction::triggered, this, &MainWindow::refreshBrowser);
    QAction *formatTool = toolbar->addAction(Icons::get(QStringLiteral("formatall.ico")),
                                             QStringLiteral("Format Current Query\tF12"));
    connect(formatTool, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->formatQuery(0);
    });
    toolbar->addSeparator();

    m_dbCombo = new QComboBox(toolbar);
    m_dbCombo->setMinimumContentsLength(22);
    toolbar->addWidget(m_dbCombo);
    connect(m_dbCombo, &QComboBox::activated, this,
            [this](int index) { useDatabaseFromCombo(m_dbCombo->itemText(index)); });
    toolbar->addSeparator();

    QAction *userMgrTool = toolbar->addAction(Icons::get(QStringLiteral("usermanager.ICO")),
                                              QStringLiteral("User Manager (Ctrl+U)"));
    connect(userMgrTool, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->promptUserManager();
    });
    toolbar->addSeparator();

    /* second-toolbar buttons reusing their menu actions, in upstream's
     * order — manage indexes / relationships need a table selected in
     * the browser and export-table-data reuses that same flow */
    /* upstream maps Export All Rows to IDI_XMLHTML_16 and Backup to
     * IDI_EXPORTDATA_16 (CreateIconList) — two distinct glyphs so the
     * menu rows read apart; the toolbar shares these actions */
    exportRows->setIcon(Icons::get(QStringLiteral("exportxmlhtml_16.ico")));
    toolbar->addAction(exportRows);
    runScript->setIcon(Icons::get(QStringLiteral("execbatch_16.ico")));
    toolbar->addAction(runScript);
    copyDb->setIcon(Icons::get(QStringLiteral("copy_data_16.ico")));
    toolbar->addAction(copyDb);
    expTblData->setIcon(Icons::get(QStringLiteral("exportxmlhtml_16.ico")));
    toolbar->addAction(expTblData);
    manageIdx->setIcon(Icons::get(QStringLiteral("manage_index_16.ico")));
    toolbar->addAction(manageIdx);
    manageFk->setIcon(Icons::get(QStringLiteral("manrel_16.ico")));
    toolbar->addAction(manageFk);
    toolbar->addSeparator();

    /* plain transaction controls, like upstream's last second-toolbar
     * group (the menu keeps the modifier variants) */
    QAction *startTrxTool =
        toolbar->addAction(Icons::get(QStringLiteral("start_transaction_16.ico")),
                           QStringLiteral("Start Transaction"));
    connect(startTrxTool, &QAction::triggered, this, [this] {
        auto *t = currentTab();
        if(!t)
            return;
        runTx(t->driverType() == SqlDriverType::Sqlite ? QStringLiteral("BEGIN")
                                                       : QStringLiteral("START TRANSACTION"));
    });
    QAction *commitTool =
        toolbar->addAction(Icons::get(QStringLiteral("commit_16.ico")), QStringLiteral("Commit"));
    connect(commitTool, &QAction::triggered, this, [this] { runTx(QStringLiteral("COMMIT")); });
    QAction *rollbackTool = toolbar->addAction(Icons::get(QStringLiteral("rollback_16.ico")),
                                               QStringLiteral("Rollback"));
    connect(rollbackTool, &QAction::triggered, this, [this] { runTx(QStringLiteral("ROLLBACK")); });

    /* the connection ("user@host:port/db") takes the left slot the static "Ready"
     * label used to hold — nothing ever updated that label, and menu-hover
     * help (QAction status tips) overlays this slot like any left-side widget */
    m_connectionLabel = new QLabel(QStringLiteral("No connection"), this);
    statusBar()->addWidget(m_connectionLabel, 1);
    m_driverLabel = new QLabel(QStringLiteral("—"), this);
    m_execLabel = new QLabel(QStringLiteral("Exec: 0 sec"), this);
    m_totalLabel = new QLabel(QStringLiteral("Total: 0 sec"), this);
    m_cursorLabel = new QLabel(QStringLiteral("Ln 1, Col 1"), this);
    m_connectionsLabel = new QLabel(QStringLiteral("Connections: 0"), this);
    for(QLabel *l : {m_driverLabel, m_connectionsLabel, m_execLabel, m_totalLabel, m_cursorLabel}) {
        l->setMinimumWidth(90);
        l->setFrameStyle(QFrame::Panel | QFrame::Sunken);
        statusBar()->addPermanentWidget(l);
    }

    /* Menu actions carry their shortcut as a "\t<keys>" hint in the text, which
     * only *displays* the accelerator. Turn each hint into a real QKeySequence
     * so the keys actually work. (Actions that already called setShortcut, or
     * whose hint isn't a parseable sequence, are left alone.) */
    std::function<void(const QList<QAction *> &)> wireShortcuts =
        [&](const QList<QAction *> &actions) {
            for(QAction *a : actions) {
                if(a->menu()) {
                    wireShortcuts(a->menu()->actions());
                    continue;
                }
                const int tab = a->text().indexOf(QLatin1Char('\t'));
                if(tab < 0 || !a->shortcut().isEmpty())
                    continue;
                const QKeySequence ks(a->text().mid(tab + 1).trimmed());
                if(!ks.isEmpty())
                    a->setShortcut(ks);
            }
        };
    wireShortcuts(menuBar()->actions());

    /* The bundled toolbar bitmaps are 16px-only and Qt never upscales an icon
     * past its largest pixmap, so the 18px buttons would just centre a tiny
     * glyph. Give each icon a smooth-scaled 18px pixmap next to the original:
     * the toolbar picks the big one, menus sharing the action keep the crisp
     * 16px. */
    for(QAction *a : toolbar->actions()) {
        const QIcon src = a->icon();
        if(src.isNull())
            continue;
        QIcon big;
        big.addPixmap(src.pixmap(16, 16));
        const QSize best =
            src.availableSizes().isEmpty() ? QSize(16, 16) : src.availableSizes().constLast();
        big.addPixmap(src.pixmap(best).scaled(kToolbarIcon, kToolbarIcon, Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation));
        a->setIcon(big);
    }

    /* Toolbar buttons duplicate a menu action's job; strip the "\t<keys>" from
     * their text (it would show literally in the tooltip) and fold it into a
     * clean tooltip — the menu owns the actual shortcut (wireShortcuts above
     * already turned the hint into a real QKeySequence, and for actions
     * shared between menu and toolbar the label must keep its "&" mnemonic,
     * so only the tooltip loses it). */
    for(QAction *a : toolbar->actions()) {
        const int tab = a->text().indexOf(QLatin1Char('\t'));
        if(tab < 0)
            continue;
        const QString name = a->text().left(tab);
        const QString keys = a->text().mid(tab + 1).trimmed();
        a->setText(name);
        QString noAmp = name;
        noAmp.remove(QLatin1Char('&'));
        a->setToolTip(keys.isEmpty() ? noAmp : QStringLiteral("%1 (%2)").arg(noAmp, keys));
    }

    syncToolbarToCurrentTab();
}
