/* OpenYog — main window: the menu bar from File through Others.
 * The class is declared in MainWindow.h; MainWindow.cpp holds the core.
 * The menus are transcribed from the upstream resource script
 * (include/SQLyog.rc, IDR_MAINMENU): labels, nesting and shortcuts are
 * verbatim. Items whose functionality hasn't been ported yet stay visible but
 * disabled — they mark the roadmap (FEATURES.md); working items are wired. */
#include "MainWindow.h"
#include "ConnectionDialog.h"
#include "ConnectionStore.h"
#include "ConnectionTab.h"
#include "FavoritesStore.h"
#include "Icons.h"
#include "ObjectBrowser.h"
#include "RecentFiles.h"

#include <QAction>
#include <QColorDialog>
#include <QHash>
#include <QFileDialog>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QKeySequence>

void MainWindow::buildFileMenu()
{
    /* ================= File (IDR_MAINMENU) ========================== */
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *newSame =
        file->addAction(QStringLiteral("New Connection Using Current Settings\tCtrl+N"));
    newSame->setIcon(Icons::get(QStringLiteral("first.ico")));
    connect(newSame, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            openAndRun([&] {
                ConnectionParams p;
                p.host = t->property("host").toString();
                p.port = t->property("port").toInt();
                p.user = t->property("user").toString();
                p.password = t->property("password").toString();
                p.database = t->property("database").toString();
                /* driverType/filePath: without these this always opened a
                 * MySQL connection regardless of the current tab's actual
                 * driver — harmless for a MySQL tab, but a SQLite tab has
                 * no meaningful host/port/user at all and a Postgres tab
                 * would get MySQL's wire protocol pointed at its host/port,
                 * so this was silently broken for both until fixed here. */
                p.driverType = static_cast<SqlDriverType>(t->property("driverType").toInt());
                p.filePath = t->property("filePath").toString();
                p.name = t->title();
                return p;
            }());
    });
    QAction *newConn = m_shared.newConn =
        file->addAction(QStringLiteral("New &Connection…\tCtrl+M"));
    connect(newConn, &QAction::triggered, this, &MainWindow::newConnection);
    file->addSeparator();
    QAction *newEditor = m_shared.newEditor =
        file->addAction(QStringLiteral("New &Query Editor\tCtrl+T"));
    connect(newEditor, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->addEditorTab();
    });
    QAction *builder = file->addAction(QStringLiteral("New Query &Builder\tCtrl+K"));
    builder->setIcon(Icons::get(QStringLiteral("entmenu.ico")));
    connect(builder, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->openQueryBuilder();
    });
    QAction *designer = file->addAction(QStringLiteral("Ne&w Schema Designer\tCtrl+Alt+D"));
    designer->setIcon(Icons::get(QStringLiteral("entmenu.ico")));
    connect(designer, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->openSchemaDesigner();
    });
    QAction *dataSearch = file->addAction(QStringLiteral("New Data Searc&h\tCtrl+Shift+D"));
    dataSearch->setIcon(Icons::get(QStringLiteral("entmenu.ico")));
    connect(dataSearch, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->promptDataSearch({});
    });
    file->addSeparator();
    QAction *closeTabAct = file->addAction(QStringLiteral("Close &Tab\tAlt+L"));
    closeTabAct->setIcon(Icons::get(QStringLiteral("closetab.ico")));
    connect(closeTabAct, &QAction::triggered, this, [this] {
        if(m_tabs->currentIndex() >= 0)
            closeTab(m_tabs->currentIndex());
    });
    QAction *renameTabAct = file->addAction(QStringLiteral("&Rename Tab\tAlt+F2"));
    renameTabAct->setIcon(Icons::get(QStringLiteral("renamequery_16.ico")));
    connect(renameTabAct, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->renameCurrentEditorTab();
    });
    QAction *disconnect = file->addAction(QStringLiteral("&Disconnect\tCtrl+F4"));
    disconnect->setIcon(Icons::get(QStringLiteral("discon.ICO")));
    connect(disconnect, &QAction::triggered, this, [this] {
        if(m_tabs->currentIndex() >= 0)
            closeTab(m_tabs->currentIndex());
    });
    QAction *disconnectAll = file->addAction(QStringLiteral("Disconnect Al&l"));
    disconnectAll->setIcon(Icons::get(QStringLiteral("disconall.ICO")));
    connect(disconnectAll, &QAction::triggered, this, [this] {
        while(m_tabs->count())
            closeTab(0);
    });
    file->addSeparator();
    QAction *openSql = file->addAction(QStringLiteral("Open…\tCtrl+O"));
    openSql->setIcon(Icons::get(QStringLiteral("open_in_new_tab.ico")));
    connect(openSql, &QAction::triggered, this, [this] {
        if(auto *tab = currentTab()) {
            const QString f =
                QFileDialog::getOpenFileName(this, QStringLiteral("Open SQL file"), QString(),
                                             QStringLiteral("SQL (*.sql);;All (*)"));
            if(!f.isEmpty())
                tab->openSqlFile(f);
        }
    });
    QAction *saveSql = file->addAction(QStringLiteral("&Save…\tCtrl+S"));
    saveSql->setIcon(Icons::get(QStringLiteral("save.ico")));
    connect(saveSql, &QAction::triggered, this, [this] {
        if(auto *tab = currentTab())
            tab->saveEditor();
    });
    QAction *saveAsAct = file->addAction(QStringLiteral("S&ave As…"));
    saveAsAct->setIcon(Icons::get(QStringLiteral("saveas.ico")));
    connect(saveAsAct, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->saveEditor();
    });
    file->addSeparator();
    /* a "session" here is just the list of open connections (name, driver,
     * host/port/user/database-or-filepath — no password, same reasoning as
     * Export Connection Details) — reopening a session reconnects each one
     * via the normal openAndRun() path. It does not restore each tab's
     * unsaved query text/editor tabs, only which servers were open. */
    const auto saveSessionTo = [this](const QString &file) {
        QJsonArray conns;
        for(int i = 0; i < m_tabs->count(); ++i) {
            if(auto *t = qobject_cast<ConnectionTab *>(m_tabs->widget(i))) {
                const ConnectionParams &p = t->params();
                QJsonObject o;
                o["name"] = p.name;
                o["driver"] = driverTypeToString(p.driverType);
                o["host"] = p.host;
                o["port"] = p.port;
                o["user"] = p.user;
                o["database"] = p.database;
                o["filepath"] = p.filePath;
                conns.append(o);
            }
        }
        QJsonObject root;
        root["connections"] = conns;
        QFile f(file);
        if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        f.write(QJsonDocument(root).toJson());
        m_sessionFile = file;
        return true;
    };
    QAction *saveSession = file->addAction(QStringLiteral("Save Session…\tCtrl+Shift+S"));
    saveSession->setIcon(Icons::get(QStringLiteral("session_save_all.ico")));
    connect(saveSession, &QAction::triggered, this, [this, saveSessionTo] {
        QString target = m_sessionFile;
        if(target.isEmpty())
            target = QFileDialog::getSaveFileName(this, QStringLiteral("Save Session"),
                                                  QStringLiteral("session.oysession"),
                                                  QStringLiteral("OpenYog session (*.oysession)"));
        if(target.isEmpty())
            return;
        if(!saveSessionTo(target))
            QMessageBox::warning(this, QStringLiteral("Save Session"),
                                 QStringLiteral("Could not write %1").arg(target));
    });
    QAction *saveSessionAs = file->addAction(QStringLiteral("Save Session As…"));
    saveSessionAs->setIcon(Icons::get(QStringLiteral("session_save_all.ico")));
    connect(saveSessionAs, &QAction::triggered, this, [this, saveSessionTo] {
        const QString target = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Session As"), QStringLiteral("session.oysession"),
            QStringLiteral("OpenYog session (*.oysession)"));
        if(target.isEmpty())
            return;
        if(!saveSessionTo(target))
            QMessageBox::warning(this, QStringLiteral("Save Session"),
                                 QStringLiteral("Could not write %1").arg(target));
    });
    QAction *openSession = file->addAction(QStringLiteral("Open Session Savepoint…\tCtrl+Shift+O"));
    openSession->setIcon(Icons::get(QStringLiteral("session_open.ico")));
    connect(openSession, &QAction::triggered, this, [this] {
        const QString target =
            QFileDialog::getOpenFileName(this, QStringLiteral("Open Session"), QString(),
                                         QStringLiteral("OpenYog session (*.oysession)"));
        if(target.isEmpty())
            return;
        QFile f(target);
        if(!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, QStringLiteral("Open Session"),
                                 QStringLiteral("Could not read %1").arg(target));
            return;
        }
        const QJsonArray conns =
            QJsonDocument::fromJson(f.readAll()).object().value("connections").toArray();
        for(const QJsonValue &v : conns) {
            const QJsonObject o = v.toObject();
            ConnectionParams p;
            p.name = o.value("name").toString(QStringLiteral("Session connection"));
            p.driverType = driverTypeFromString(o.value("driver").toString());
            p.host = o.value("host").toString(p.host);
            p.port = o.value("port").toInt(p.port);
            p.user = o.value("user").toString();
            p.database = o.value("database").toString();
            p.filePath = o.value("filepath").toString();
            /* the saved connection of that name, if it's the same server,
             * has the password (keychain); otherwise ask — any server
             * connection, not SQLite's file */
            ConnectionParams saved;
            const bool known = ConnectionStore::load(p.name, &saved) &&
                               saved.driverType == p.driverType && saved.host == p.host &&
                               saved.port == p.port && saved.user == p.user;
            if(known) {
                p = saved;
                p.database = o.value("database").toString(saved.database);
            } else if(p.driverType != SqlDriverType::Sqlite) {
                bool ok = false;
                p.password = QInputDialog::getText(
                    this, QStringLiteral("Open Session"),
                    QStringLiteral("Password for %1@%2 (left blank if none):").arg(p.user, p.host),
                    QLineEdit::Password, QString(), &ok);
                if(!ok)
                    continue;
            }
            openAndRun(p);
        }
        m_sessionFile = target;
    });
    QAction *endSession = file->addAction(QStringLiteral("End Session\tCtrl+Shift+X"));
    endSession->setIcon(Icons::get(QStringLiteral("session_close.ico")));
    connect(endSession, &QAction::triggered, this, [this] {
        while(m_tabs->count())
            closeTab(0);
        m_sessionFile.clear();
    });
    file->addSeparator();
    QMenu *recent = file->addMenu(QStringLiteral("&Recent Files"));
    recent->menuAction()->setIcon(Icons::get(QStringLiteral("foldericon.ico")));
    /* rebuilt on every open (Favorites uses the same trick): upstream's
     * recent list is also plain "&N path" rows read back from the ini,
     * ten max (FrameWindow::InsertRecentMenuItems) */
    connect(recent, &QMenu::aboutToShow, this, [this, recent] {
        for(QAction *a : recent->actions())
            if(a->data().toBool())
                recent->removeAction(a);
        const QStringList files = RecentFiles::load();
        if(files.isEmpty()) {
            QAction *none = recent->addAction(QStringLiteral("(no recent files)"));
            none->setEnabled(false);
            none->setData(true);
            return;
        }
        for(int i = 0; i < files.size(); ++i) {
            const QString path = files.at(i);
            QAction *a = recent->addAction(QStringLiteral("&%1 %2").arg(i + 1).arg(path));
            a->setData(true);
            connect(a, &QAction::triggered, this, [this, path] {
                if(auto *t = currentTab())
                    t->openSqlFile(path);
            });
        }
    });
    file->addSeparator();
    QAction *quit = file->addAction(QStringLiteral("E&xit\tAlt+F4"));
    quit->setIcon(Icons::get(QStringLiteral("exit.ico")));
    connect(quit, &QAction::triggered, this, &MainWindow::close);
}

void MainWindow::buildEditMenu()
{
    /* ================= Edit ========================================= */
    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    QAction *refresh = edit->addAction(QStringLiteral("Refresh &Object Browser\tF5"));
    refresh->setIcon(Icons::get(QStringLiteral("refresh_16.ico")));
    connect(refresh, &QAction::triggered, this, &MainWindow::refreshBrowser);
    QAction *browserColor = edit->addAction(QStringLiteral("Change Objec&t Browser Color"));
    browserColor->setIcon(Icons::get(QStringLiteral("colorpicker.ico")));
    connect(browserColor, &QAction::triggered, this, [this] {
        const QColor cur = ObjectBrowserColor::load();
        const QColor c = QColorDialog::getColor(cur.isValid() ? cur : QColor(Qt::white), this,
                                                QStringLiteral("Object Browser Selection Color"));
        if(!c.isValid())
            return;
        ObjectBrowserColor::save(c);
        QMessageBox::information(
            this, QStringLiteral("Object Browser Color"),
            QStringLiteral("Saved — applies to connections opened from now on."));
    });
    QAction *collapseAll = edit->addAction(QStringLiteral("Collapse All in Object Browser"));
    collapseAll->setIcon(Icons::get(QStringLiteral("object.ico")));
    connect(collapseAll, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->collapseBrowser();
    });
    /* owner-requested toggle over the tree's double-click gesture, backed
     * by upstream's own GetTextOnDBClick key (see ObjectBrowserDoubleClick).
     * Checked = double-click opens Table Data directly; unchecked = the
     * upstream default, the object's name drops into the editor. The
     * Preferences dialog writes the same key, so re-sync on every open. */
    QAction *openDataOnDbl = m_shared.openDataOnDbl =
        edit->addAction(QStringLiteral("Open Table Data on Dou&ble-Click"));
    /* checkable → no icon: Qt paints EITHER the glyph or the check
     * indicator in the icon column, never both — the tick is the point
     * here (upstream's checkable rows are icon-less for the same reason).
     * State is set here and re-synced from the Preferences save below —
     * NOT in QMenu::aboutToShow, which a plain show() never emits */
    openDataOnDbl->setCheckable(true);
    openDataOnDbl->setChecked(!ObjectBrowserDoubleClick::pasteNameInEditor());
    connect(openDataOnDbl, &QAction::triggered, this, [openDataOnDbl] {
        ObjectBrowserDoubleClick::setPasteNameInEditor(!openDataOnDbl->isChecked());
    });
    edit->addSeparator();
    QMenu *execMenu = edit->addMenu(QStringLiteral("Execute Quer&y"));
    execMenu->menuAction()->setIcon(Icons::get(QStringLiteral("execute_16.ico")));
    QAction *execQuery = execMenu->addAction(QStringLiteral("Exe&cute Query\tF9"));
    execQuery->setIcon(Icons::get(QStringLiteral("execute_16.ico")));
    connect(execQuery, &QAction::triggered, this, &MainWindow::executeCurrentTab);
    QAction *execAll = execMenu->addAction(QStringLiteral("Execute &All Queries\tCtrl+F9"));
    execAll->setIcon(Icons::get(QStringLiteral("execall_16.ico")));
    connect(execAll, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->runAll();
    });
    QAction *execEditMenu = execMenu->addAction(QStringLiteral("Execute And Edit &Resultset\tF8"));
    execEditMenu->setIcon(Icons::get(QStringLiteral("execforupd_16.ico")));
    connect(execEditMenu, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->runAndEdit();
    });
    QMenu *explain = edit->addMenu(QStringLiteral("Execute Explain"));
    explain->menuAction()->setIcon(Icons::get(QStringLiteral("tablediag.ico")));
    connect(explain->addAction(QStringLiteral("&EXPLAIN Current Query")), &QAction::triggered, this,
            [this] {
                if(auto *t = currentTab())
                    t->explainCurrent(false);
            });
    connect(explain->addAction(QStringLiteral("EXPLAIN &FORMAT=JSON")), &QAction::triggered, this,
            [this] {
                if(auto *t = currentTab())
                    t->explainCurrent(true);
            });
    edit->addSeparator();
    QMenu *formatter = edit->addMenu(QStringLiteral("S&QL Formatter"));
    formatter->menuAction()->setIcon(Icons::get(QStringLiteral("formatall.ico")));
    const auto wireFormat = [this, formatter](const QString &label, QKeySequence sc, int scope) {
        QAction *a = formatter->addAction(label);
        a->setShortcut(sc);
        connect(a, &QAction::triggered, this, [this, scope] {
            if(auto *t = currentTab())
                t->formatQuery(scope);
        });
    };
    wireFormat(QStringLiteral("Format &Current Query\tF12"), QKeySequence(Qt::Key_F12), 0);
    wireFormat(QStringLiteral("Format &Selected Query\tCtrl+F12"),
               QKeySequence(Qt::CTRL | Qt::Key_F12), 1);
    wireFormat(QStringLiteral("Format &All Queries\tShift+F12"),
               QKeySequence(Qt::SHIFT | Qt::Key_F12), 2);
    QMenu *insertTpl = edit->addMenu(QStringLiteral("&Insert Templates…\tCtrl+Shift+T"));
    insertTpl->menuAction()->setIcon(Icons::get(QStringLiteral("templatesm.ico")));
    for(const auto &[label, kind] :
        {std::pair{QStringLiteral("&INSERT INTO <tablename>…"), 0},
         std::pair{QStringLiteral("&UPDATE <tablename> SET…"), 1},
         std::pair{QStringLiteral("&DELETE FROM <tablename>…"), 2},
         std::pair{QStringLiteral("&SELECT <col-1>…<col-n> FROM…"), 3}}) {
        connect(insertTpl->addAction(label), &QAction::triggered, this, [this, kind] {
            if(auto *t = currentTab())
                t->pasteSqlTemplate(kind);
        });
    }
    edit->addSeparator();
    QAction *undo = edit->addAction(QStringLiteral("&Undo\tCtrl+Z"));
    undo->setIcon(Icons::get(QStringLiteral("undo.ico")));
    connect(undo, &QAction::triggered, this, [this] { editClipboard(QStringLiteral("undo")); });
    QAction *redo = edit->addAction(QStringLiteral("&Redo\tCtrl+Y"));
    redo->setIcon(Icons::get(QStringLiteral("redo.ico")));
    connect(redo, &QAction::triggered, this, [this] { editClipboard(QStringLiteral("redo")); });
    edit->addSeparator();
    QAction *cut = edit->addAction(QStringLiteral("Cu&t\tCtrl+X"));
    cut->setIcon(Icons::get(QStringLiteral("cut.ico")));
    connect(cut, &QAction::triggered, this, [this] { editClipboard(QStringLiteral("cut")); });
    QAction *copy = edit->addAction(QStringLiteral("&Copy\tCtrl+C"));
    copy->setIcon(Icons::get(QStringLiteral("Copy.ico")));
    connect(copy, &QAction::triggered, this, [this] { editClipboard(QStringLiteral("copy")); });
    QAction *copyNorm = edit->addAction(QStringLiteral("Copy With Normalized &Whitespace\tAlt+C"));
    copyNorm->setIcon(Icons::get(QStringLiteral("text_View.ico")));
    connect(copyNorm, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->editorCopyNormalizedWhitespace();
    });
    QAction *paste = edit->addAction(QStringLiteral("&Paste\tCtrl+V"));
    paste->setIcon(Icons::get(QStringLiteral("paste.ico")));
    connect(paste, &QAction::triggered, this, [this] { editClipboard(QStringLiteral("paste")); });
    QAction *insertFromFile = edit->addAction(QStringLiteral("Insert From Fi&le…"));
    insertFromFile->setIcon(Icons::get(QStringLiteral("import_data.ico")));
    connect(insertFromFile, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->editorInsertFromFile();
    });
    QAction *selAll = edit->addAction(QStringLiteral("Select &All\tCtrl+A"));
    selAll->setIcon(Icons::get(QStringLiteral("grid_view.ico")));
    connect(selAll, &QAction::triggered, this,
            [this] { editClipboard(QStringLiteral("selectall")); });
    edit->addSeparator();
    const auto onEditor = [this](void (ConnectionTab::*fn)()) {
        if(auto *t = currentTab())
            (t->*fn)();
    };
    QAction *findAct = edit->addAction(QStringLiteral("&Find…\tCtrl+F"));
    findAct->setIcon(Icons::get(QStringLiteral("search.ico")));
    findAct->setShortcut(QKeySequence::Find);
    connect(findAct, &QAction::triggered, this,
            [onEditor] { onEditor(&ConnectionTab::promptFind); });
    QAction *findNextAct = edit->addAction(QStringLiteral("Find Next\tF3"));
    findNextAct->setIcon(Icons::get(QStringLiteral("search.ico")));
    findNextAct->setShortcut(QKeySequence::FindNext);
    connect(findNextAct, &QAction::triggered, this,
            [onEditor] { onEditor(&ConnectionTab::findNext); });
    QAction *replaceAct = edit->addAction(QStringLiteral("R&eplace…\tCtrl+H"));
    replaceAct->setIcon(Icons::get(QStringLiteral("replace.ico")));
    replaceAct->setShortcut(QKeySequence::Replace);
    connect(replaceAct, &QAction::triggered, this,
            [onEditor] { onEditor(&ConnectionTab::promptReplace); });
    QAction *gotoAct = edit->addAction(QStringLiteral("&Go To…\tCtrl+G"));
    gotoAct->setIcon(Icons::get(QStringLiteral("arrow_down.ico")));
    gotoAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    connect(gotoAct, &QAction::triggered, this,
            [onEditor] { onEditor(&ConnectionTab::promptGoto); });
    edit->addSeparator();
    QAction *listTags = edit->addAction(QStringLiteral("Li&st All Tags\tCtrl+Space"));
    listTags->setIcon(Icons::get(QStringLiteral("entmenu.ico")));
    /* no setShortcut — the editor handles Ctrl+Space itself */
    connect(listTags, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->listTags();
    });
    QAction *listMatching = edit->addAction(QStringLiteral("List &Matching Tags\tCtrl+Enter"));
    listMatching->setIcon(Icons::get(QStringLiteral("entmenu.ico")));
    connect(listMatching, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->listTags();
    });
    edit->addSeparator();
    QAction *hideBrowser = edit->addAction(QStringLiteral("Hide Object &Browser\tCtrl+Shift+1"));
    hideBrowser->setIcon(Icons::get(QStringLiteral("closetab.ico")));
    connect(hideBrowser, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->toggleBrowserPane();
    });
    QAction *hideResult = edit->addAction(QStringLiteral("Hide Result Pa&ne\tCtrl+Shift+2"));
    hideResult->setIcon(Icons::get(QStringLiteral("closetab.ico")));
    connect(hideResult, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->toggleResultPane();
    });
    QAction *hideEditor = edit->addAction(QStringLiteral("&Hide SQL Editor\tCtrl+Shift+3"));
    hideEditor->setIcon(Icons::get(QStringLiteral("closetab.ico")));
    connect(hideEditor, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->toggleEditorPane();
    });
    QAction *prevTab = edit->addAction(QStringLiteral("Switch To Pre&vious Tab\tCtrl+PgUp"));
    prevTab->setIcon(Icons::get(QStringLiteral("Up.ico")));
    connect(prevTab, &QAction::triggered, this, [this] { switchTab(-1); });
    QAction *nextTab = edit->addAction(QStringLiteral("Switch To Ne&xt Tab\tCtrl+PgDown"));
    nextTab->setIcon(Icons::get(QStringLiteral("Down.ico")));
    connect(nextTab, &QAction::triggered, this, [this] { switchTab(1); });
    edit->addSeparator();
    QMenu *advanced = edit->addMenu(QStringLiteral("A&dvanced"));
    advanced->menuAction()->setIcon(Icons::get(QStringLiteral("preferences.ico")));
    QAction *upper = advanced->addAction(QStringLiteral("Make Selection &Uppercase\tCtrl+Shift+U"));
    connect(upper, &QAction::triggered, this, [this] { editClipboard(QStringLiteral("upper")); });
    QAction *lower = advanced->addAction(QStringLiteral("Make Selection &Lowercase\tCtrl+Shift+L"));
    connect(lower, &QAction::triggered, this, [this] { editClipboard(QStringLiteral("lower")); });
    advanced->addSeparator();
    QAction *commentAct = advanced->addAction(QStringLiteral("&Comment Selection\tCtrl+Shift+C"));
    commentAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    connect(commentAct, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->commentSelection(true);
    });
    QAction *uncommentAct =
        advanced->addAction(QStringLiteral("&Remove Comment From Selection\tCtrl+Shift+R"));
    uncommentAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    connect(uncommentAct, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->commentSelection(false);
    });
}

void MainWindow::buildFavoritesMenu()
{
    /* ================= Favorites ==================================== */
    QMenu *favorites = menuBar()->addMenu(QStringLiteral("Fa&vorites"));
    QAction *addFav = favorites->addAction(QStringLiteral("&Add To Favorites…\tCtrl+Shift+F"));
    addFav->setIcon(Icons::get(QStringLiteral("addtofavorite.ico")));
    addFav->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    connect(addFav, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->addCurrentToFavorites();
    });
    QAction *organizeFav = favorites->addAction(QStringLiteral("&Organize Favorites…"));
    organizeFav->setIcon(Icons::get(QStringLiteral("favorites.ico")));
    connect(organizeFav, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->organizeFavorites();
    });
    favorites->addSeparator();
    /* rebuilt every time the menu opens, so Add/Rename/Delete are reflected
     * immediately without a separate "Refresh" step */
    connect(favorites, &QMenu::aboutToShow, this, [this, favorites] {
        for(QAction *a : favorites->actions())
            if(a->data().toBool())
                favorites->removeAction(a);
        const QStringList names = FavoritesStore::names();
        if(names.isEmpty()) {
            QAction *none = favorites->addAction(QStringLiteral("(no favorites saved)"));
            none->setEnabled(false);
            none->setData(true);
            return;
        }
        for(const QString &name : names) {
            QAction *a = favorites->addAction(name, this, [this, name] {
                if(auto *t = currentTab())
                    t->insertFavorite(name);
            });
            a->setData(true); /* marks it as dynamic, for removal above */
        }
    });
}

void MainWindow::buildDatabaseMenu()
{
    /* ================= Database ===================================== */
    QMenu *database = menuBar()->addMenu(QStringLiteral("&Database"));
    QAction *copyDb = m_shared.copyDb =
        database->addAction(QStringLiteral("&Copy Database To Different Host/Database…"));
    connect(copyDb, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->promptCopyDatabase();
        else
            QMessageBox::information(this, QStringLiteral("Copy Database"),
                                     QStringLiteral("Open a connection first."));
    });
    QAction *createDb = database->addAction(QStringLiteral("Create &Database…\tCtrl+D"));
    createDb->setIcon(Icons::get(QStringLiteral("new_data.ico")));
    connect(createDb, &QAction::triggered, this, &MainWindow::createDatabase);
    QAction *alterDb = database->addAction(QStringLiteral("&Alter Database…\tF6"));
    alterDb->setIcon(Icons::get(QStringLiteral("alterdb.ico")));
    connect(alterDb, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->promptAlterDatabase({});
    });
    QMenu *create = database->addMenu(QStringLiteral("C&reate"));
    create->menuAction()->setIcon(Icons::get(QStringLiteral("new_data.ico")));
    QAction *createTblFromDb = create->addAction(QStringLiteral("&Table"));
    createTblFromDb->setIcon(Icons::get(QStringLiteral("createtableindata.ico")));
    connect(createTblFromDb, &QAction::triggered, this, [this] { createTable(); });
    /* icon per schema-object kind, matching upstream's Database > Create submenu
     * (ID_DB_CREATEVIEW/CREATESTOREDPROCEDURE/CREATEFUNCTION/CREATETRIGGER/
     * CREATEEVENT in CreateIconList) */
    const QHash<QString, QString> createIcons = {
        {QStringLiteral("VIEW"), QStringLiteral("viewnew.ico")},
        {QStringLiteral("PROCEDURE"), QStringLiteral("addsp.ico")},
        {QStringLiteral("FUNCTION"), QStringLiteral("addfunction.ico")},
        {QStringLiteral("TRIGGER"), QStringLiteral("addtrigger.ico")},
        {QStringLiteral("EVENT"), QStringLiteral("eventnew.ico")},
    };
    for(const auto &pair :
        {std::pair<QString, QString>{QStringLiteral("&View…"), QStringLiteral("VIEW")},
         {QStringLiteral("&Stored Procedure…"), QStringLiteral("PROCEDURE")},
         {QStringLiteral("&Function…"), QStringLiteral("FUNCTION")},
         {QStringLiteral("Tri&gger…"), QStringLiteral("TRIGGER")},
         {QStringLiteral("&Event…"), QStringLiteral("EVENT")}}) {
        const QString kw = pair.second;
        QAction *a = create->addAction(pair.first);
        a->setIcon(Icons::get(createIcons.value(kw)));
        connect(a, &QAction::triggered, this, [this, kw] {
            if(auto *t = currentTab())
                t->createSchemaObject({}, kw);
        });
    }
    QMenu *dbOps = database->addMenu(QStringLiteral("More Database &Operations"));
    dbOps->menuAction()->setIcon(Icons::get(QStringLiteral("database.ico")));
    QAction *dropDbAct = dbOps->addAction(QStringLiteral("Dro&p Database…\tDel"));
    dropDbAct->setIcon(Icons::get(QStringLiteral("dropdatabase.ico")));
    connect(dropDbAct, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->dropDatabase({});
    });
    QAction *truncDbAct = dbOps->addAction(QStringLiteral("Tr&uncate Database…\tShift+Del"));
    truncDbAct->setIcon(Icons::get(QStringLiteral("truncatedata.ico")));
    connect(truncDbAct, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->truncateDatabase({});
    });
    QAction *emptyDbAct = dbOps->addAction(QStringLiteral("E&mpty Database…"));
    emptyDbAct->setIcon(Icons::get(QStringLiteral("emptydata.ico")));
    connect(emptyDbAct, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->emptyDatabase({});
    });
    database->addSeparator();
    QMenu *dbBackup = database->addMenu(QStringLiteral("&Backup/Export"));
    dbBackup->menuAction()->setIcon(Icons::get(QStringLiteral("backup.ico")));
    addDisabled(dbBackup, QStringLiteral("&Scheduled Backups…\tCtrl+Alt+S"));
    QAction *dbDump =
        dbBackup->addAction(QStringLiteral("&Backup Database As SQL Dump…\tCtrl+Alt+E"));
    dbDump->setIcon(Icons::get(QStringLiteral("export_data_16.ico")));
    connect(dbDump, &QAction::triggered, this, [this] { dumpDatabase(); });
    QMenu *dbImport = database->addMenu(QStringLiteral("&Import "));
    dbImport->menuAction()->setIcon(Icons::get(QStringLiteral("import_data.ico")));
    dbImport->addAction(importExternalAction());
    QAction *dbRunScript =
        dbImport->addAction(QStringLiteral("&Execute SQL Script…\tCtrl+Shift+Q"));
    dbRunScript->setIcon(Icons::get(QStringLiteral("execbatch_16.ico")));
    connect(dbRunScript, &QAction::triggered, this, [this] {
        if(auto *t = currentTab()) {
            const QString f =
                QFileDialog::getOpenFileName(this, QStringLiteral("Execute SQL script"), QString(),
                                             QStringLiteral("SQL (*.sql);;All (*)"));
            if(!f.isEmpty())
                t->openSqlFile(f);
        }
    });
    database->addSeparator();
    QAction *schemaHtml = database->addAction(
        QStringLiteral("Create Schema For Database In &HTML…\tCtrl+Shift+Alt+S"));
    schemaHtml->setIcon(Icons::get(QStringLiteral("schema.ico")));
    connect(schemaHtml, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->promptSchemaHtml({});
    });
}

/* runs fn on the object browser's selected table, or says to select one */
void MainWindow::onSelectedTable(void (ConnectionTab::*fn)(const QString &, const QString &))
{
    auto *tab = currentTab();
    if(!tab) {
        return;
    }
    const QStringList info = tab->selectedTableInfo();
    if(info.size() < 2) {
        QMessageBox::information(this, QStringLiteral("OpenYog"),
                                 QStringLiteral("Select a table in the object browser first."));
        return;
    }
    (tab->*fn)(info[0], info[1]);
}

void MainWindow::buildTableMenu()
{
    /* ================= Table ======================================== */
    QMenu *table = menuBar()->addMenu(QStringLiteral("T&able"));
    QMenu *pasteSql = table->addMenu(QStringLiteral("&Paste SQL Statement"));
    pasteSql->menuAction()->setIcon(Icons::get(QStringLiteral("templatesm.ico")));
    QAction *pasteIns =
        pasteSql->addAction(QStringLiteral("&INSERT INTO <tablename>…\tAlt+Shift+I"));
    QAction *pasteUpd =
        pasteSql->addAction(QStringLiteral("&UPDATE <tablename> SET…\tAlt+Shift+U"));
    QAction *pasteDel =
        pasteSql->addAction(QStringLiteral("&DELETE FROM <tablename>…\tAlt+Shift+D"));
    QAction *pasteSel =
        pasteSql->addAction(QStringLiteral("&SELECT <col-1>…<col-n> FROM…\tAlt+Shift+S"));
    connect(pasteIns, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->pasteSqlTemplate(0);
    });
    connect(pasteUpd, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->pasteSqlTemplate(1);
    });
    connect(pasteDel, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->pasteSqlTemplate(2);
    });
    connect(pasteSel, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->pasteSqlTemplate(3);
    });
    QAction *copyTableHost =
        table->addAction(QStringLiteral("&Copy Table(s) To Different Host/Database…"));
    copyTableHost->setIcon(Icons::get(QStringLiteral("copy_data.ico")));
    connect(copyTableHost, &QAction::triggered, this, &MainWindow::copySelectedTableToHost);
    table->addSeparator();
    QAction *openTable = table->addAction(QStringLiteral("&Open Table\tF11"));
    openTable->setIcon(Icons::get(QStringLiteral("viewdata.ico")));
    connect(openTable, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->openSelectedTable();
    });
    QAction *openTableNewTab = table->addAction(QStringLiteral("Open Table in &New Tab\tCtrl+F11"));
    openTableNewTab->setIcon(Icons::get(QStringLiteral("open_in_new_tab.ico")));
    connect(openTableNewTab, &QAction::triggered, this, [this] {
        auto *t = currentTab();
        if(!t)
            return;
        const QStringList info = t->selectedTableInfo();
        if(info.size() < 2) {
            QMessageBox::information(this, QStringLiteral("OpenYog"),
                                     QStringLiteral("Select a table in the object browser first."));
            return;
        }
        /* a second connection to the same server/file, not a second view
         * onto the same connection — the app has exactly one TableDataView
         * per tab, so "new tab" means "new connection tab" here */
        ConnectionParams p = t->params();
        p.database = info[0];
        if(openAndRun(p)) {
            if(auto *newTab = currentTab())
                newTab->openTableData(info[0], info[1]);
        }
    });
    QAction *createTbl = table->addAction(QStringLiteral("Create &Table\tF4"));
    createTbl->setIcon(Icons::get(QStringLiteral("createtableindata.ico")));
    createTbl->setShortcut(QKeySequence(Qt::Key_F4));
    connect(createTbl, &QAction::triggered, this, [this] { createTable(); });
    QAction *alterTbl = table->addAction(QStringLiteral("&Alter Table\tF6"));
    alterTbl->setIcon(Icons::get(QStringLiteral("altertable.ico")));
    alterTbl->setShortcut(QKeySequence(Qt::Key_F6));
    connect(alterTbl, &QAction::triggered, this, &MainWindow::alterTable);
    QAction *manageIdx = m_shared.manageIdx =
        table->addAction(QStringLiteral("&Manage Indexes\tF7"));
    manageIdx->setShortcut(QKeySequence(Qt::Key_F7));
    connect(manageIdx, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptManageIndexes); });
    QAction *manageFk = m_shared.manageFk =
        table->addAction(QStringLiteral("Re&lationships/Foreign Keys\tF10"));
    manageFk->setShortcut(QKeySequence(Qt::Key_F10));
    connect(manageFk, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptManageForeignKeys); });
    QMenu *moreTable = table->addMenu(QStringLiteral("Mo&re Table Operations"));
    moreTable->menuAction()->setIcon(Icons::get(QStringLiteral("Addtables.ico")));
    QAction *renameTbl = moreTable->addAction(QStringLiteral("&Rename Table\tF2"));
    renameTbl->setIcon(Icons::get(QStringLiteral("rename.ico")));
    renameTbl->setShortcut(QKeySequence(Qt::Key_F2));
    connect(renameTbl, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptRenameTable); });
    QAction *truncTbl = moreTable->addAction(QStringLiteral("Tru&ncate Table…\tShift+Del"));
    truncTbl->setIcon(Icons::get(QStringLiteral("emptytable.ico")));
    connect(truncTbl, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::truncateTable); });
    QAction *dropTbl = moreTable->addAction(QStringLiteral("&Drop Table From Database…\tDel"));
    dropTbl->setIcon(Icons::get(QStringLiteral("drop_table.ico")));
    connect(dropTbl, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::dropTable); });
    /* upstream's Reorder Column(s) is exactly this: open the table's
     * column editor in alter mode (FrameWindow: CreateTableTabInterface
     * …TABCOLUMNS — its dedicated CReorderColumn dialog is commented out
     * there too) — Move Up/Down + FIRST/AFTER generation already live in
     * CreateTableDialog::buildAlterSql */
    QAction *reorderCols = moreTable->addAction(QStringLiteral("Re&order Column(s)\tCtrl+Alt+R"));
    reorderCols->setIcon(Icons::get(QStringLiteral("reordercol.ico")));
    reorderCols->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_R));
    connect(reorderCols, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptAlterTable); });
    QAction *dupTbl = moreTable->addAction(QStringLiteral("Duplicate Table &Structure/Data…"));
    dupTbl->setIcon(Icons::get(QStringLiteral("copytable.ICO")));
    connect(dupTbl, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptCopyTable); });
    QAction *tblProps = moreTable->addAction(QStringLiteral("View &Table Properties"));
    tblProps->setIcon(Icons::get(QStringLiteral("tableprop.ico")));
    connect(tblProps, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::showTableProperties); });
    table->addSeparator();
    QMenu *tblBackup = table->addMenu(QStringLiteral("&Backup/Export"));
    tblBackup->menuAction()->setIcon(Icons::get(QStringLiteral("backup.ico")));
    addDisabled(tblBackup, QStringLiteral("&Scheduled Backups…\tCtrl+Alt+S"));
    QAction *dumpTblAct =
        tblBackup->addAction(QStringLiteral("&Backup Table(s) As SQL Dump…\tCtrl+Alt+E"));
    dumpTblAct->setIcon(Icons::get(QStringLiteral("export_data_16.ico")));
    connect(dumpTblAct, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::dumpTable); });
    QAction *expTblData = m_shared.expTblData =
        tblBackup->addAction(QStringLiteral("&Export Table Data As…\tCtrl+Alt+C"));
    connect(expTblData, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::exportTableData); });
    QMenu *tblImport = table->addMenu(QStringLiteral("&Import "));
    tblImport->menuAction()->setIcon(Icons::get(QStringLiteral("import_data.ico")));
    tblImport->addAction(importExternalAction());
    QAction *importCsv =
        tblImport->addAction(QStringLiteral("&Import CSV Data Using LOAD LOCAL…\tCtrl+Shift+M"));
    importCsv->setIcon(Icons::get(QStringLiteral("csv.ico")));
    connect(importCsv, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptImportCsv); });
    QAction *importXml =
        tblImport->addAction(QStringLiteral("Import &XML Data Using LOAD LOCAL…\tCtrl+Shift+X"));
    connect(importXml, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptImportXml); });
    table->addSeparator();
    QAction *createTrig = table->addAction(QStringLiteral("Create Tri&gger…"));
    createTrig->setIcon(Icons::get(QStringLiteral("addtrigger.ico")));
    connect(createTrig, &QAction::triggered, this, [this] {
        auto *t = currentTab();
        if(!t)
            return;
        const QStringList info = t->selectedTableInfo();
        /* same generic CREATE TRIGGER template Database > Create > Trigger…
         * uses — just pre-scoped to the selected table's database when one
         * is selected in the browser, since that's the common case here */
        t->createSchemaObject(info.size() >= 1 ? info[0] : QString(), QStringLiteral("TRIGGER"));
    });
}

void MainWindow::buildOthersMenu()
{
    QAction *manageIdx = m_shared.manageIdx;
    /* ================= Others ======================================= */
    QMenu *others = menuBar()->addMenu(QStringLiteral("&Others"));
    QMenu *columns = others->addMenu(QStringLiteral("&Columns"));
    columns->menuAction()->setIcon(Icons::get(QStringLiteral("column.ico")));
    QAction *dropCol = columns->addAction(QStringLiteral("Drop &Column…\tDel"));
    dropCol->setIcon(Icons::get(QStringLiteral("dropcolumn.ico")));
    connect(dropCol, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptDropColumn); });
    QAction *manageCols = columns->addAction(QStringLiteral("&Manage Columns\tF6"));
    manageCols->setIcon(Icons::get(QStringLiteral("column.ico")));
    connect(manageCols, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptAlterTable); });
    QMenu *indexes = others->addMenu(QStringLiteral("&Indexes"));
    indexes->menuAction()->setIcon(Icons::get(QStringLiteral("manage_index_16.ico")));
    QAction *createIdx = indexes->addAction(QStringLiteral("Create &Index\tF4"));
    createIdx->setIcon(Icons::get(QStringLiteral("indexcreate.ico")));
    connect(createIdx, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptManageIndexes); });
    QAction *editIdx = indexes->addAction(QStringLiteral("&Edit Index\tF6"));
    editIdx->setIcon(Icons::get(QStringLiteral("indexedit.ico")));
    connect(editIdx, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptManageIndexes); });
    /* upstream also has "&Drop Index...\tDel" here (its own dedicated
     * command, ID_INDEXES_DROPINDEX) — like Create/Edit above, it opens
     * the same unified list/add/drop dialog rather than a separate
     * drop-only flow, since that's the only index-management entry point
     * this port has. Manage Indexes\tF7 follows it, a separator between —
     * exactly upstream's structure — reusing the Table menu's own action
     * (manageIdx) rather than a second QAction for the identical command. */
    QAction *dropIdx = indexes->addAction(QStringLiteral("&Drop Index…\tDel"));
    dropIdx->setIcon(Icons::get(QStringLiteral("indexdelete.ico")));
    connect(dropIdx, &QAction::triggered, this,
            [this] { onSelectedTable(&ConnectionTab::promptManageIndexes); });
    indexes->addSeparator();
    indexes->addAction(manageIdx);
}
