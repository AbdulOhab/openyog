/* OpenYog — main window.
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
#include "Theme.h"
#include "db/IDbConnection.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QFileDialog>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QFrame>
#include <QKeySequence>
#include <QStatusBar>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QToolBar>
#include <QVBoxLayout>

#include <functional>

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

    /* app icon at the left of the menu bar — visible regardless of whether the
     * window manager draws a title-bar icon (SQLyog shows one here too) */
    auto *menuIcon = new QLabel(this);
    menuIcon->setPixmap(QPixmap(QStringLiteral(":/resources/openyog-24.png")));
    menuIcon->setContentsMargins(6, 2, 6, 2);
    menuBar()->setCornerWidget(menuIcon, Qt::TopLeftCorner);

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
     * pane was just a blank rectangle; SQLyog's MDI area is the blue strip */
    auto *welcome = new QWidget(this);
    welcome->setStyleSheet(QStringLiteral("background:#3B7DBB;"));
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
                p.driverType = static_cast<DriverType>(t->property("driverType").toInt());
                p.filePath = t->property("filePath").toString();
                p.name = t->title();
                return p;
            }());
    });
    QAction *newConn = file->addAction(QStringLiteral("New &Connection…\tCtrl+M"));
    connect(newConn, &QAction::triggered, this, &MainWindow::newConnection);
    file->addSeparator();
    QAction *newEditor = file->addAction(QStringLiteral("New &Query Editor\tCtrl+T"));
    connect(newEditor, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->addEditorTab();
    });
    addDisabled(file, QStringLiteral("New Query &Builder\tCtrl+K"));
    addDisabled(file, QStringLiteral("Ne&w Schema Designer\tCtrl+Alt+D"));
    QAction *dataSearch = file->addAction(QStringLiteral("New Data Searc&h\tCtrl+Shift+D"));
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
            if(p.driverType == DriverType::Mysql) {
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
    recent->addAction(QStringLiteral("(no recent files)"))->setEnabled(false);
    file->addSeparator();
    QAction *quit = file->addAction(QStringLiteral("E&xit\tAlt+F4"));
    quit->setIcon(Icons::get(QStringLiteral("exit.ico")));
    connect(quit, &QAction::triggered, this, &MainWindow::close);

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
    connect(edit->addAction(QStringLiteral("Collapse All in Object Browser")), &QAction::triggered,
            this, [this] {
                if(auto *t = currentTab())
                    t->collapseBrowser();
            });
    edit->addSeparator();
    QMenu *execMenu = edit->addMenu(QStringLiteral("Execute Quer&y"));
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
    connect(edit->addAction(QStringLiteral("Copy With Normalized &Whitespace\tAlt+C")),
            &QAction::triggered, this, [this] {
                if(auto *t = currentTab())
                    t->editorCopyNormalizedWhitespace();
            });
    QAction *paste = edit->addAction(QStringLiteral("&Paste\tCtrl+V"));
    paste->setIcon(Icons::get(QStringLiteral("paste.ico")));
    connect(paste, &QAction::triggered, this, [this] { editClipboard(QStringLiteral("paste")); });
    connect(edit->addAction(QStringLiteral("Insert From Fi&le…")), &QAction::triggered, this,
            [this] {
                if(auto *t = currentTab())
                    t->editorInsertFromFile();
            });
    QAction *selAll = edit->addAction(QStringLiteral("Select &All\tCtrl+A"));
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
    findNextAct->setShortcut(QKeySequence::FindNext);
    connect(findNextAct, &QAction::triggered, this,
            [onEditor] { onEditor(&ConnectionTab::findNext); });
    QAction *replaceAct = edit->addAction(QStringLiteral("R&eplace…\tCtrl+H"));
    replaceAct->setIcon(Icons::get(QStringLiteral("replace.ico")));
    replaceAct->setShortcut(QKeySequence::Replace);
    connect(replaceAct, &QAction::triggered, this,
            [onEditor] { onEditor(&ConnectionTab::promptReplace); });
    QAction *gotoAct = edit->addAction(QStringLiteral("&Go To…\tCtrl+G"));
    gotoAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    connect(gotoAct, &QAction::triggered, this,
            [onEditor] { onEditor(&ConnectionTab::promptGoto); });
    edit->addSeparator();
    QAction *listTags = edit->addAction(QStringLiteral("Li&st All Tags\tCtrl+Space"));
    /* no setShortcut — the editor handles Ctrl+Space itself */
    connect(listTags, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->listTags();
    });
    QAction *listMatching = edit->addAction(QStringLiteral("List &Matching Tags\tCtrl+Enter"));
    connect(listMatching, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->listTags();
    });
    edit->addSeparator();
    QAction *hideBrowser = edit->addAction(QStringLiteral("Hide Object &Browser\tCtrl+Shift+1"));
    connect(hideBrowser, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->toggleBrowserPane();
    });
    QAction *hideResult = edit->addAction(QStringLiteral("Hide Result Pa&ne\tCtrl+Shift+2"));
    connect(hideResult, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->toggleResultPane();
    });
    QAction *hideEditor = edit->addAction(QStringLiteral("&Hide SQL Editor\tCtrl+Shift+3"));
    connect(hideEditor, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->toggleEditorPane();
    });
    QAction *prevTab = edit->addAction(QStringLiteral("Switch To Pre&vious Tab\tCtrl+PgUp"));
    connect(prevTab, &QAction::triggered, this, [this] { switchTab(-1); });
    QAction *nextTab = edit->addAction(QStringLiteral("Switch To Ne&xt Tab\tCtrl+PgDown"));
    connect(nextTab, &QAction::triggered, this, [this] { switchTab(1); });
    edit->addSeparator();
    QMenu *advanced = edit->addMenu(QStringLiteral("A&dvanced"));
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

    /* ================= Database ===================================== */
    QMenu *database = menuBar()->addMenu(QStringLiteral("&Database"));
    QAction *copyDb =
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
    addDisabled(dbBackup, QStringLiteral("&Scheduled Backups…\tCtrl+Alt+S"));
    QAction *dbDump =
        dbBackup->addAction(QStringLiteral("&Backup Database As SQL Dump…\tCtrl+Alt+E"));
    dbDump->setIcon(Icons::get(QStringLiteral("export_data_16.ico")));
    connect(dbDump, &QAction::triggered, this, [this] { dumpDatabase(); });
    QMenu *dbImport = database->addMenu(QStringLiteral("&Import "));
    addDisabled(dbImport, QStringLiteral("Import E&xternal Data…\tCtrl+Alt+O"));
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

    /* ================= Table ======================================== */
    QMenu *table = menuBar()->addMenu(QStringLiteral("T&able"));
    QMenu *pasteSql = table->addMenu(QStringLiteral("&Paste SQL Statement"));
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
    /* each of these acts on the object browser's selected table */
    const auto onSelectedTable = [this](
                                     void (ConnectionTab::*fn)(const QString &, const QString &)) {
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
    };
    QAction *manageIdx = table->addAction(QStringLiteral("&Manage Indexes\tF7"));
    manageIdx->setShortcut(QKeySequence(Qt::Key_F7));
    connect(manageIdx, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptManageIndexes); });
    QAction *manageFk = table->addAction(QStringLiteral("Re&lationships/Foreign Keys\tF10"));
    manageFk->setShortcut(QKeySequence(Qt::Key_F10));
    connect(manageFk, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptManageForeignKeys); });
    QMenu *moreTable = table->addMenu(QStringLiteral("Mo&re Table Operations"));
    QAction *renameTbl = moreTable->addAction(QStringLiteral("&Rename Table\tF2"));
    renameTbl->setIcon(Icons::get(QStringLiteral("rename.ico")));
    renameTbl->setShortcut(QKeySequence(Qt::Key_F2));
    connect(renameTbl, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptRenameTable); });
    QAction *truncTbl = moreTable->addAction(QStringLiteral("Tru&ncate Table…\tShift+Del"));
    truncTbl->setIcon(Icons::get(QStringLiteral("emptytable.ico")));
    connect(truncTbl, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::truncateTable); });
    QAction *dropTbl = moreTable->addAction(QStringLiteral("&Drop Table From Database…\tDel"));
    dropTbl->setIcon(Icons::get(QStringLiteral("drop_table.ico")));
    connect(dropTbl, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::dropTable); });
    addDisabled(moreTable, QStringLiteral("Re&order Column(s)\tCtrl+Alt+R"))
        ->setIcon(Icons::get(QStringLiteral("reordercol.ico")));
    QAction *dupTbl = moreTable->addAction(QStringLiteral("Duplicate Table &Structure/Data…"));
    dupTbl->setIcon(Icons::get(QStringLiteral("copytable.ICO")));
    connect(dupTbl, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptCopyTable); });
    QAction *tblProps = moreTable->addAction(QStringLiteral("View &Table Properties"));
    tblProps->setIcon(Icons::get(QStringLiteral("tableprop.ico")));
    connect(tblProps, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::showTableProperties); });
    table->addSeparator();
    QMenu *tblBackup = table->addMenu(QStringLiteral("&Backup/Export"));
    addDisabled(tblBackup, QStringLiteral("&Scheduled Backups…\tCtrl+Alt+S"));
    QAction *dumpTblAct =
        tblBackup->addAction(QStringLiteral("&Backup Table(s) As SQL Dump…\tCtrl+Alt+E"));
    dumpTblAct->setIcon(Icons::get(QStringLiteral("export_data_16.ico")));
    connect(dumpTblAct, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::dumpTable); });
    QAction *expTblData =
        tblBackup->addAction(QStringLiteral("&Export Table Data As…\tCtrl+Alt+C"));
    connect(expTblData, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::exportTableData); });
    QMenu *tblImport = table->addMenu(QStringLiteral("&Import "));
    addDisabled(tblImport, QStringLiteral("Import E&xternal Data…\tCtrl+Alt+O"));
    QAction *importCsv =
        tblImport->addAction(QStringLiteral("&Import CSV Data Using LOAD LOCAL…\tCtrl+Shift+M"));
    importCsv->setIcon(Icons::get(QStringLiteral("csv.ico")));
    connect(importCsv, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptImportCsv); });
    QAction *importXml =
        tblImport->addAction(QStringLiteral("Import &XML Data Using LOAD LOCAL…\tCtrl+Shift+X"));
    connect(importXml, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptImportXml); });
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

    /* ================= Others ======================================= */
    QMenu *others = menuBar()->addMenu(QStringLiteral("&Others"));
    QMenu *columns = others->addMenu(QStringLiteral("&Columns"));
    QAction *dropCol = columns->addAction(QStringLiteral("Drop &Column…\tDel"));
    dropCol->setIcon(Icons::get(QStringLiteral("dropcolumn.ico")));
    connect(dropCol, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptDropColumn); });
    QAction *manageCols = columns->addAction(QStringLiteral("&Manage Columns\tF6"));
    manageCols->setIcon(Icons::get(QStringLiteral("column.ico")));
    connect(manageCols, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptAlterTable); });
    QMenu *indexes = others->addMenu(QStringLiteral("&Indexes"));
    QAction *createIdx = indexes->addAction(QStringLiteral("Create &Index\tF4"));
    createIdx->setIcon(Icons::get(QStringLiteral("indexcreate.ico")));
    connect(createIdx, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptManageIndexes); });
    QAction *editIdx = indexes->addAction(QStringLiteral("&Edit Index\tF6"));
    editIdx->setIcon(Icons::get(QStringLiteral("indexedit.ico")));
    connect(editIdx, &QAction::triggered, this,
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptManageIndexes); });
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
            [onSelectedTable] { onSelectedTable(&ConnectionTab::promptManageIndexes); });
    indexes->addSeparator();
    indexes->addAction(manageIdx);

    /* ================= Tools ======================================== */
    QMenu *tools = menuBar()->addMenu(QStringLiteral("&Tools"));
    QAction *exportRows =
        tools->addAction(QStringLiteral("&Export All Rows Of Table Data/Result As…\tCtrl+Shift+E"));
    connect(exportRows, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->exportCurrent();
    });
    QAction *toolsDump =
        tools->addAction(QStringLiteral("&Backup Database As SQL Dump…\tCtrl+Alt+E"));
    toolsDump->setIcon(Icons::get(QStringLiteral("export_data_16.ico")));
    connect(toolsDump, &QAction::triggered, this, [this] { dumpDatabase(); });
    QAction *runScript = tools->addAction(QStringLiteral("Execute &SQL Script…\tCtrl+Shift+Q"));
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
        if(t->driverType() != DriverType::Mysql) {
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
            [onSelectedTable] { onSelectedTable(&ConnectionTab::tableDiagnostics); });
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
    QMenu *show = tools->addMenu(QStringLiteral("Sho&w"));
    /* SQLite has none of these concepts at all (no server, no runtime
     * parameters, no other connections to list) — guarded with a plain
     * message; PostgreSQL has real equivalents, just different statements,
     * not MySQL's SHOW syntax at all. */
    const auto showFor = [this](const QString &mysqlSql, const QString &pgSql,
                                const QString &tabTitle) {
        auto *t = currentTab();
        if(!t)
            return;
        if(t->driverType() == DriverType::Sqlite) {
            QMessageBox::information(this, tabTitle,
                                     QStringLiteral("SQLite has no server to show this for."));
            return;
        }
        t->runStatements(QStringList{t->driverType() == DriverType::Postgres ? pgSql : mysqlSql},
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
    addDisabled(tools, QStringLiteral("Change &Language\tAlt+Shift+L"));
    QMenu *connDetails = tools->addMenu(QStringLiteral("Export/I&mport Connection Details"));
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
        if(p.driverType == DriverType::Mysql) {
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
    connect(preferences, &QAction::triggered, this, [this] {
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
        QColor browserColor = ObjectBrowserColor::load();
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
        form->addRow(QStringLiteral("Object browser selection color"), colorBtn);
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
        ObjectBrowserColor::save(browserColor);
    });
    QAction *queryTimeout = tools->addAction(QStringLiteral("Query &Timeout…"));
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

    /* ================= Powertools =================================== */
    QMenu *powertools = menuBar()->addMenu(QStringLiteral("&Powertools"));
    addDisabled(powertools, QStringLiteral("Database S&ynchronization Wizard…\tCtrl+Alt+W"));
    addDisabled(powertools, QStringLiteral("&Visual Data Comparison Wizard…\tCtrl+Alt+Q"));
    addDisabled(powertools, QStringLiteral("Schema &Synchronization Tool…\tCtrl+Q"));
    powertools->addSeparator();
    addDisabled(powertools, QStringLiteral("Import E&xternal Data…\tCtrl+Alt+O"));
    addDisabled(powertools, QStringLiteral("S&QL Scheduler and Reporting Wizard…\tCtrl+Alt+N"));
    addDisabled(powertools, QStringLiteral("S&cheduled Backups…\tCtrl+Alt+S"));
    powertools->addSeparator();
    addDisabled(powertools, QStringLiteral("Scheduled &Jobs…"));
    powertools->addSeparator();
    QAction *rebuildTags = powertools->addAction(QStringLiteral("&Rebuild tags"));
    connect(rebuildTags, &QAction::triggered, this, [this] {
        if(auto *t = currentTab())
            t->refreshBrowser(); /* re-syncs the object tree + autocomplete tags */
    });

    /* ================= Transactions ================================= */
    QMenu *transactions = menuBar()->addMenu(QStringLiteral("T&ransactions"));
    const auto runTx = [this](const QString &sql) {
        if(auto *t = currentTab())
            t->runStatements(QStringList{sql}, QStringLiteral("Transaction"));
    };
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
        if(t->driverType() != DriverType::Mysql) {
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
        if(t->driverType() == DriverType::Sqlite) {
            QMessageBox::information(this, QStringLiteral("Transactions"),
                                     QStringLiteral("Not supported on SQLite."));
            return;
        }
        t->runStatements(QStringList{sql}, QStringLiteral("Transaction"));
    };

    QAction *autocommit = transactions->addAction(QStringLiteral("Set Autocommit"));
    autocommit->setCheckable(true);
    autocommit->setChecked(true);
    connect(autocommit, &QAction::toggled, this, [mysqlOnlyTx](bool on) {
        mysqlOnlyTx(QStringLiteral("SET autocommit=%1").arg(on ? 1 : 0));
    });

    QMenu *isolation = transactions->addMenu(QStringLiteral("Isolation Level"));
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
    QAction *startPlain = startTrx->addAction(QStringLiteral("With no modifier"));
    connect(startPlain, &QAction::triggered, this, [this, runTx] {
        auto *t = currentTab();
        if(!t)
            return;
        runTx(t->driverType() == DriverType::Sqlite ? QStringLiteral("BEGIN")
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
        notSqliteTx(t->driverType() == DriverType::Postgres
                        ? QStringLiteral("START TRANSACTION READ ONLY")
                        : QStringLiteral("START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY"));
    });
    QAction *snapRW = snapshot->addAction(QStringLiteral("Read Write"));
    connect(snapRW, &QAction::triggered, this, [this, notSqliteTx] {
        auto *t = currentTab();
        if(!t)
            return;
        notSqliteTx(t->driverType() == DriverType::Postgres
                        ? QStringLiteral("START TRANSACTION READ WRITE")
                        : QStringLiteral("START TRANSACTION WITH CONSISTENT SNAPSHOT, READ WRITE"));
    });

    QMenu *commit = transactions->addMenu(QStringLiteral("Commit"));
    QAction *commitPlain = commit->addAction(QStringLiteral("With no modifier"));
    connect(commitPlain, &QAction::triggered, this, [runTx] { runTx(QStringLiteral("COMMIT")); });
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
    connect(rollTrx, &QAction::triggered, this, [runTx] { runTx(QStringLiteral("ROLLBACK")); });

    /* ================= Window ======================================= */
    QMenu *window = menuBar()->addMenu(QStringLiteral("&Window"));
    QAction *winClose = window->addAction(QStringLiteral("&Close Tab\tAlt+L"));
    winClose->setIcon(Icons::get(QStringLiteral("closetab.ico")));
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
    toolbar->setIconSize(QSize(16, 16));

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
    exportRows->setIcon(Icons::get(QStringLiteral("export_data_16.ico")));
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
    connect(startTrxTool, &QAction::triggered, this, [this, runTx] {
        auto *t = currentTab();
        if(!t)
            return;
        runTx(t->driverType() == DriverType::Sqlite ? QStringLiteral("BEGIN")
                                                    : QStringLiteral("START TRANSACTION"));
    });
    QAction *commitTool =
        toolbar->addAction(Icons::get(QStringLiteral("commit_16.ico")), QStringLiteral("Commit"));
    connect(commitTool, &QAction::triggered, this, [runTx] { runTx(QStringLiteral("COMMIT")); });
    QAction *rollbackTool = toolbar->addAction(Icons::get(QStringLiteral("rollback_16.ico")),
                                               QStringLiteral("Rollback"));
    connect(rollbackTool, &QAction::triggered, this,
            [runTx] { runTx(QStringLiteral("ROLLBACK")); });

    m_statusMsg = new QLabel(QStringLiteral("Ready"), this);
    statusBar()->addWidget(m_statusMsg, 1);
    m_driverLabel = new QLabel(QStringLiteral("—"), this);
    m_connectionLabel = new QLabel(QStringLiteral("No connection"), this);
    m_execLabel = new QLabel(QStringLiteral("Exec: 0 sec"), this);
    m_totalLabel = new QLabel(QStringLiteral("Total: 0 sec"), this);
    m_cursorLabel = new QLabel(QStringLiteral("Ln 1, Col 1"), this);
    m_connectionsLabel = new QLabel(QStringLiteral("Connections: 0"), this);
    for(QLabel *l : {m_driverLabel, m_connectionLabel, m_execLabel, m_totalLabel, m_cursorLabel,
                     m_connectionsLabel}) {
        l->setMinimumWidth(90);
        l->setFrameStyle(QFrame::Panel | QFrame::Sunken);
        statusBar()->addPermanentWidget(l);
    }
    m_connectionLabel->setMinimumWidth(170);

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
    return live;
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
        m_driverLabel->setText(driverDisplayName(tab->driverType()));
        m_connectionLabel->setText(tab->activeConnectionLabel());
    } else {
        setWindowTitle(QStringLiteral("OpenYog"));
        m_driverLabel->setText(QStringLiteral("—"));
        m_connectionLabel->setText(QStringLiteral("No connection"));
    }
    /* counts real server connections (a tab that has browsed another
     * PostgreSQL database holds several at once) — it used to count
     * tabs, which read as a lie the moment a side connection opened */
    m_connectionsLabel->setText(QStringLiteral("Connections: %1").arg(totalLiveConnections()));
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
    if(tab->driverType() == DriverType::Sqlite) {
        /* a SQLite "database" is a file — there's no CREATE DATABASE
         * statement to run against an existing connection at all; a new
         * database means a new connection to a new file */
        QMessageBox::information(this, QStringLiteral("Create Database"),
                                 QStringLiteral("SQLite has no CREATE DATABASE — a database is "
                                                "just a file. Use File → New Connection (Ctrl+M) "
                                                "and pick a new .sqlite file path instead."));
        return;
    }
    const bool pg = tab->driverType() == DriverType::Postgres;
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
    tab->runQuery(); /* run the editor's default query so the grid has data */
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

bool MainWindow::selftestCopyDb(const QString &src, const QString &tgt)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    QString err;
    const bool ok = tab->copyDatabaseTo(src, tgt, true, true, true, &err);
    if(!ok)
        qWarning("copydb failed: %s", qPrintable(err));
    return ok;
}

bool MainWindow::selftestCopyDbPostgres(const QString &src, const QString &tgt)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    QString err;
    const bool ok = tab->copyDatabaseToPostgres(src, tgt, true, true, true, &err);
    if(!ok)
        qWarning("pgcopydb failed: %s", qPrintable(err));
    return ok;
}

bool MainWindow::selftestMultiDb(const QString &otherDb)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    int fails = 0;
    const auto check = [&](bool ok, const QString &what) {
        if(!ok)
            ++fails;
        QTextStream(stdout) << "pgmultidbtest " << what << (ok ? "  PASS\n" : "  FAIL\n");
    };

    const QString primary = tab->currentDatabase();
    QString err1, err2, err3;
    IDbConnection *primaryViaEmpty = tab->connectionFor(QString(), &err1);
    IDbConnection *primaryViaName = tab->connectionFor(primary, &err2);
    check(primaryViaEmpty && primaryViaEmpty == primaryViaName,
          QStringLiteral(
              "connectionFor(\"\")/connectionFor(primary) both resolve to the same connection"));

    IDbConnection *side1 = tab->connectionFor(otherDb, &err3);
    check(side1 != nullptr, QStringLiteral("connectionFor(\"%1\") succeeds").arg(otherDb));
    if(!side1) {
        QTextStream(stdout) << "  error: " << err3 << '\n';
        return fails == 0;
    }
    check(side1 != primaryViaEmpty, QStringLiteral("side connection differs from the primary one"));

    QString err4;
    IDbConnection *side2 = tab->connectionFor(otherDb, &err4);
    check(side1 == side2,
          QStringLiteral("a second connectionFor() call for the same db reuses the cached one"));

    /* the side connection must genuinely see the OTHER database's own
     * catalog, not the primary's (the whole point of this feature) */
    const QStringList tables =
        side1->listTables(QStringLiteral("public"), QStringLiteral("BASE TABLE"));
    check(!tables.isEmpty(),
          QStringLiteral("side connection lists tables in %1.public").arg(otherDb));
    QTextStream(stdout) << "  tables: " << tables.join(QStringLiteral(", ")) << '\n';

    const ConnectionParams p = tab->paramsFor(otherDb);
    check(p.database == otherDb && p.host == tab->params().host && p.user == tab->params().user &&
              p.driverType == tab->driverType(),
          QStringLiteral("paramsFor() carries the right database with the same host/user/driver"));

    /* end-to-end: this is what "Connect to <db> in New Tab" actually does
     * (ObjectBrowser::openDatabaseInNewTabRequested -> ConnectionTab::
     * newTabRequested -> this same openAndRun call, wired in openAndRun()
     * itself) — verify it produces a second, correctly-scoped tab rather
     * than just trusting the two already-tested halves compose correctly */
    const int tabsBefore = m_tabs->count();
    const bool opened = openAndRun(p);
    check(opened && m_tabs->count() == tabsBefore + 1,
          QStringLiteral("\"Connect in New Tab\" opens exactly one new tab"));
    if(opened) {
        /* currentDatabase(), not defaultDb() — the latter is the *schema*
         * ("public" until the user picks another one), unrelated to which
         * physical database the new tab actually connected to */
        auto *newTab = qobject_cast<ConnectionTab *>(m_tabs->widget(m_tabs->count() - 1));
        check(
            newTab && newTab->currentDatabase() == otherDb,
            QStringLiteral("the new tab is scoped to %1, not the original database").arg(otherDb));
    }

    return fails == 0;
}

bool MainWindow::selftestCopySqliteFile(const QString &target, bool withData)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    QString err;
    const bool ok = tab->copySqliteFileTo(target, withData, &err);
    if(!ok)
        qWarning("sqlitecopydb failed: %s", qPrintable(err));
    return ok;
}

bool MainWindow::selftestSchemaHtml(const QString &outFile)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    const QString db = tab->defaultDb().isEmpty() ? QStringLiteral("main") : tab->defaultDb();
    const QString html = tab->buildSchemaHtml(db);
    QFile f(outFile);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("schemahtmltest: could not write %s", qPrintable(outFile));
        return false;
    }
    f.write(html.toUtf8());
    return true;
}

bool MainWindow::selftestCsvImportBatched(const QString &db, const QString &file,
                                          const QString &table, const QString &onDup)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    int rows = 0;
    QString err;
    const bool ok =
        tab->importCsvBatched(db, table, file, QStringLiteral(","), QStringLiteral("\""),
                              QStringLiteral("\\"), true, 0, false, onDup, &rows, &err);
    if(!ok)
        qWarning("csvimport failed: %s", qPrintable(err));
    else
        qInfo("csvimport: %d row(s) inserted into %s", rows, qPrintable(table));
    return ok;
}

void MainWindow::openTableData(const QString &db, const QString &table)
{
    if(auto *tab = currentTab())
        tab->openTableData(db, table);
}

void MainWindow::setDataViewMode(const QString &mode)
{
    if(auto *tab = currentTab())
        tab->setDataViewMode(mode);
}

void MainWindow::openSchemaObjectTab(const QString &objType)
{
    if(auto *tab = currentTab())
        tab->createSchemaObject({}, objType);
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

void MainWindow::selftestUseDatabase(const QString &db)
{
    if(auto *tab = currentTab())
        tab->useDatabase(db);
}

void MainWindow::selftestExpandDatabase(const QString &name)
{
    if(auto *tab = currentTab())
        tab->expandDatabaseNode(name);
}

void MainWindow::selftestRunAgain()
{
    executeCurrentTab();
}

void MainWindow::selftestExplain(const QString &mode)
{
    if(auto *tab = currentTab())
        tab->selftestExplain(mode);
}

void MainWindow::selftestShowInfoTab()
{
    if(auto *tab = currentTab())
        tab->selftestShowInfoTab();
}

void MainWindow::selftestSelectBrowserItem(const QString &path)
{
    if(auto *tab = currentTab())
        tab->selectBrowserItem(path);
}

void MainWindow::selftestClickBrowserItem(const QString &path)
{
    if(auto *tab = currentTab())
        tab->clickBrowserItem(path);
}

void MainWindow::selftestCollapseBrowserItem(const QString &path)
{
    if(auto *tab = currentTab())
        tab->collapseBrowserItem(path);
}

void MainWindow::selftestExpandBrowserItem(const QString &path)
{
    if(auto *tab = currentTab())
        tab->expandBrowserItem(path);
}

QStringList MainWindow::selftestDumpTree(const QString &path)
{
    if(auto *tab = currentTab())
        return tab->dumpBrowserSubtree(path);
    return {};
}

QStringList MainWindow::selftestTreeMenu(const QString &path)
{
    if(auto *tab = currentTab())
        return tab->browserContextMenuItems(path);
    return {};
}

void MainWindow::selftestDoubleClickBrowserItem(const QString &path)
{
    if(auto *tab = currentTab())
        tab->doubleClickBrowserItem(path);
}

QStringList MainWindow::selftestComboItems() const
{
    /* [0] is the combo's current selection — "(none)" when
     * currentIndex() is -1 (nothing picked yet), matching what the combo
     * visually shows (blank) in that state — followed by every item */
    QStringList out;
    out << (m_dbCombo->currentIndex() < 0 ? QStringLiteral("(none)") : m_dbCombo->currentText());
    for(int i = 0; i < m_dbCombo->count(); ++i)
        out << m_dbCombo->itemText(i);
    return out;
}

void MainWindow::selftestSwitchDatabase(const QString &db)
{
    /* switching databases is the Object Browser tree's job (double-click
     * a database node, or its "Switch to `db`" menu entry) — the toolbar
     * combo only ever lists schemas, so this calls ConnectionTab's own
     * method directly rather than driving tree widget items, which is
     * exactly what either of those real interactions ends up calling */
    if(auto *tab = currentTab())
        tab->switchDatabase(db);
}

int MainWindow::selftestTabCount() const
{
    return m_tabs->count();
}

void MainWindow::selftestPickDropdownSchema(const QString &schema)
{
    m_dbCombo->setCurrentText(schema);
}

void MainWindow::closeTab(int index)
{
    QWidget *w = m_tabs->widget(index);
    m_tabs->removeTab(index);
    delete w;
    syncToolbarToCurrentTab();
}
