/* OpenYog — application entry point.
 *
 * Selftest mode for CI/headless verification:
 *   openyog --screenshot=FILE.png              render main window to FILE, exit
 *   openyog --screenshot=FILE.png --dialog     render the connection dialog
 *   openyog --screenshot=FILE.png --dialog --dialogdriver=sqlite|postgres   … driver preselected
 *   openyog --screenshot=FILE.png --createtable render the Create Table dialog
 *   openyog --autoconnect=h:p:u:pw:db [--opentable=db:t] [--editcell=r:c:v |
 *           --stagecell=r:c:v] --screenshot=FILE.png   drive the data grid
 *   openyog --autoconnect=… --dumpdb=FILE.sql          dump a database
 *   openyog --delconn=NAME                             delete a saved connection
 *   openyog --screenshot=FILE.png --indexdlg           render the Manage Indexes dialog
 *   openyog --screenshot=FILE.png --fkdlg              render the Foreign Keys dialog
 *   openyog --fmtsql="SELECT …"                        print the formatted SQL, exit
 *   openyog --comptest                                 autocomplete self-check
 *   openyog --sqlitetest=FILE.sqlite                   SQLite driver shape self-check
 *   openyog --pgtest=host:port:user:pw:db              PostgreSQL driver shape self-check
 *   openyog --autoconnectfile=FILE.sqlite [--opentable=db:t] [--editcell=r:c:v]
 *           --screenshot=FILE.png       same data-grid selftests, SQLite driver
 *   openyog --autoconnect…=… --schemadesigner[=FILE.png] --screenshot=FILE.png
 *           open the Schema Designer, print its tables and links
 *   openyog --autoconnectpg=h:p:u:pw:db --explain=json|plain --screenshot=FILE.png
 *           … same downstream machinery as --autoconnect=, PostgreSQL driver
 */
#include "MainWindow.h"
#include "PrivateFiles.h"
#include "Selftest.h"
#include "ConnectionDialog.h"
#include "ConnectionParams.h"
#include "ConnectionStore.h"
#include "CreateTableDialog.h"
#include "ForeignKeyDialog.h"
#include "IndexDialog.h"
#include "ExportDialog.h"
#include "SqlDumpDialog.h"
#include "SqlFormat.h"
#include "CustomFilterDialog.h"
#include "Theme.h"
#include "db/IDbDriver.h"
#include "db/IDbConnection.h"
#include "db/SshTunnel.h"

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QTabWidget>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>

#include <memory>

int main(int argc, char *argv[])
{
    /* --version: for packagers and bug reports, before any Qt setup */
    for(int i = 1; i < argc; ++i)
        if(qstrcmp(argv[i], "--version") == 0) {
            QTextStream(stdout) << "OpenYog " << OPENYOG_VERSION << '\n';
            return 0;
        }

    QString screenshot;
    bool shotDialog = false;
    QString dialogDriver; /* --dialogdriver=sqlite : preselect a driver, --dialog selftest */
    QString dialogTab;    /* --dialogtab=SSH : show that tab, --dialog selftest */
    bool shotCreateTable = false;
    bool shotIndexDlg = false;
    bool shotFkDlg = false;   /* --fkdlg: render the Foreign Keys dialog */
    bool shotDumpDlg = false; /* --dumpdlg: render the SQL-dump options dialog */
    bool shotExportDlg = false;
    QPair<QString, QString> openTableParts;
    QString dataViewMode;  /* --dataview=text|grid selftest */
    QString checkRows;     /* --checkrows=0,2,4 selftest */
    QString hexCell;       /* --hexcell=row:col:hexdigits selftest */
    QStringList cellTexts; /* --celltext=row:col selftest (repeatable): print the cell's displayed
                            * text — asserts the <BLOB> placeholder for binary columns, the
                            * post-edit display, … */
    QStringList tdMenus;   /* --tdmenu=row:col selftest (repeatable): dump the Table Data
                            * context menu's labels + enabled states over that cell */
    QString mkObj;         /* --mkobj=VIEW|PROCEDURE|… selftest */
    QString explainMode;   /* --explain=json|plain selftest: Explain "SELECT 1" */
    bool showDataTab = false;    /* --showdatatab selftest: switch to Table Data, print its table */
    bool schemaDesigner = false; /* --schemadesigner[=FILE.png] selftest: open File ▸ New Schema
                                  * Designer, print its tables/links (and export the image) */
    QString schemaDesignerExport;
    bool printResults = false;  /* --printresults selftest: Messages and result tabs, at the end */
    QString schemaSyncSpec;     /* --schemasync=SOURCE:TARGET[:apply][:shot=F] selftest */
    QString queryBuilderSpec;   /* --querybuilder=table:col,col+table:col selftest: add
                                 * them to a Query Builder, print and run its SQL */
    QString infoSearch;         /* --infosearch=TEXT selftest: type it into the Info tab's search */
    bool showInfoTab = false;   /* --showinfotab selftest: switch to the Info result tab */
    QString useDbArg;           /* --usedb=NAME selftest */
    QString expandPgDbArg;      /* --expandpgdb=NAME selftest */
    bool runAgain = false;      /* --runagain selftest: F9 a second time before the screenshot */
    QString sessionTestArg;     /* --sessiontest=FILE.sqlite selftest */
    bool sshTrust = false;      /* --sshtrust selftest: accept an unknown SSH host key */
    QString editorSql;          /* --sql=TEXT selftest: put TEXT in the editor (with --runagain:
                                 * run it) */
    QStringList completeAts;    /* --completeat=TEXT selftest (repeatable): print what
                                 * autocomplete offers for TEXT, caret at its end */
    bool copyTableHost = false; /* --copytablehost selftest: Table ▸ Copy Table(s) To Different
                                   Host… (non-MySQL-source guard) */
    QString selectTreePath;     /* --selecttreepath=a/b/c selftest */
    QString clickTreePath;      /* --clicktreepath=a/b/c selftest (synthesized mouse click) */
    QString dblClickTreePath;   /* --dblclicktreepath=a/b/c selftest (synthesized double-click) */
    QString collapseTreePath;   /* --collapsetreepath=a/b/c selftest */
    QString expandTreePath; /* --expandtreepath=a/b/c selftest (setExpanded(true), not a click) */
    QStringList
        dumpTreePaths; /* --dumptree=a/b/c selftest: print that node's subtree (repeatable) */
    QStringList treeMenuPaths; /* --treemenu=a/b/c selftest: print that node's right-click
                                * context menu item texts (repeatable) — compares what
                                * MySQL/SQLite/PostgreSQL offer for the same node kind
                                * without eyeballing a screenshot */
    bool dumpEditor = false;   /* --dumpeditor selftest: print the active editor's text */
    /* the four tree gestures above, repeated --xxxtreepath= args accumulate
     * here in command-line order and are replayed in that order — lets a
     * selftest script a whole browse sequence (expand A, double-click B,
     * expand C…) instead of one gesture per run */
    QList<QPair<QString, QString>> treeActions;
    int editRow = -1, editCol = -1;
    bool stageOnly = false;
    QString editValue;
    ConnectionParams autoConnect;
    bool doAutoConnect = false;
    QString dumpPath;
    QString copyDbArg;
    QString pgCopyDbArg;
    QString pgMultiDbArg;
    bool dumpCombo = false; /* --dumpcombo selftest */
    QString switchDbArg;    /* --switchdb=NAME selftest: switch database like the tree does */
    QString pickSchemaArg;  /* --pickschema=NAME selftest: pick a schema like a real combo click */
    QString sqliteCopyDbArg;
    QString sqliteCsvImportArg;
    QString pgCsvImportArg;
    QString schemaHtmlArg;
    QString delConn;
    QString shotMenuArg;  /* --shotmenu=Title selftest: screenshot one open top-level menu */
    QString previewTheme; /* --previewtheme=dark|twilight|light selftest: apply live,
                           * without touching the persisted OpenYog.ini choice */
    for(int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if(a.startsWith(QStringLiteral("--screenshot=")))
            screenshot = a.mid(QStringLiteral("--screenshot=").size());
        if(a == QStringLiteral("--dialog"))
            shotDialog = true;
        if(a.startsWith(QStringLiteral("--dialogdriver=")))
            dialogDriver = a.mid(QStringLiteral("--dialogdriver=").size());
        if(a.startsWith(QStringLiteral("--dialogtab=")))
            dialogTab = a.mid(QStringLiteral("--dialogtab=").size());
        if(a == QStringLiteral("--createtable"))
            shotCreateTable = true;
        if(a == QStringLiteral("--indexdlg"))
            shotIndexDlg = true;
        if(a == QStringLiteral("--fkdlg"))
            shotFkDlg = true;
        if(a == QStringLiteral("--dumpdlg"))
            shotDumpDlg = true;
        if(a == QStringLiteral("--runagain"))
            runAgain = true;
        /* --ssh=host:port:user:password [--sshkey=FILE]: route the
         * --autoconnect=/--autoconnectpg= connection through an SSH tunnel
         * (the database host:port then as seen from the SSH host) */
        if(a.startsWith(QStringLiteral("--ssh="))) {
            const QStringList s = a.mid(QStringLiteral("--ssh=").size()).split(QLatin1Char(':'));
            if(s.size() == 4) {
                autoConnect.useSsh = true;
                autoConnect.sshHost = s[0];
                autoConnect.sshPort = s[1].toInt();
                autoConnect.sshUser = s[2];
                autoConnect.sshPassword = s[3];
            }
        }
        if(a.startsWith(QStringLiteral("--sshkey=")))
            autoConnect.sshKeyFile = a.mid(QStringLiteral("--sshkey=").size());
        if(a == QStringLiteral("--sshtrust"))
            sshTrust = true;
        if(a.startsWith(QStringLiteral("--sessiontest=")))
            sessionTestArg = a.mid(QStringLiteral("--sessiontest=").size());
        if(a.startsWith(QStringLiteral("--sql=")))
            editorSql = a.mid(QStringLiteral("--sql=").size());
        if(a.startsWith(QStringLiteral("--completeat=")))
            completeAts << a.mid(QStringLiteral("--completeat=").size());
        if(a == QStringLiteral("--copytablehost"))
            copyTableHost = true;
        if(a.startsWith(QStringLiteral("--selecttreepath=")))
            selectTreePath = a.mid(QStringLiteral("--selecttreepath=").size());
        if(a.startsWith(QStringLiteral("--clicktreepath="))) {
            clickTreePath = a.mid(QStringLiteral("--clicktreepath=").size());
            treeActions.append({QStringLiteral("click"), clickTreePath});
        }
        if(a.startsWith(QStringLiteral("--dblclicktreepath="))) {
            dblClickTreePath = a.mid(QStringLiteral("--dblclicktreepath=").size());
            treeActions.append({QStringLiteral("dblclick"), dblClickTreePath});
        }
        if(a.startsWith(QStringLiteral("--collapsetreepath="))) {
            collapseTreePath = a.mid(QStringLiteral("--collapsetreepath=").size());
            treeActions.append({QStringLiteral("collapse"), collapseTreePath});
        }
        if(a.startsWith(QStringLiteral("--expandtreepath="))) {
            expandTreePath = a.mid(QStringLiteral("--expandtreepath=").size());
            treeActions.append({QStringLiteral("expand"), expandTreePath});
        }
        if(a.startsWith(QStringLiteral("--dumptree=")))
            dumpTreePaths.append(a.mid(QStringLiteral("--dumptree=").size()));
        if(a == QStringLiteral("--dumpeditor"))
            dumpEditor = true;
        if(a.startsWith(QStringLiteral("--treemenu=")))
            treeMenuPaths.append(a.mid(QStringLiteral("--treemenu=").size()));
        if(a == QStringLiteral("--exportdlg"))
            shotExportDlg = true;
        /* --opentable=db:table (selftest: opens the editable data grid) */
        /* --editcell=row:col:value  stage the edit AND apply it (UPDATE path) */
        /* --stagecell=row:col:value stage only (shows the amber cell + Apply bar) */
        if(a.startsWith(QStringLiteral("--editcell=")) ||
           a.startsWith(QStringLiteral("--stagecell="))) {
            stageOnly = a.startsWith(QStringLiteral("--stagecell="));
            const QStringList parts = a.section('=', 1).split(':');
            if(parts.size() == 3) {
                editRow = parts[0].toInt();
                editCol = parts[1].toInt();
                editValue = parts[2];
            }
        }
        if(a.startsWith(QStringLiteral("--dumpdb=")))
            dumpPath = a.mid(QStringLiteral("--dumpdb=").size());
        if(a.startsWith(QStringLiteral("--copydb=")))
            copyDbArg = a.mid(QStringLiteral("--copydb=").size());
        if(a.startsWith(QStringLiteral("--pgcopydb=")))
            pgCopyDbArg = a.mid(QStringLiteral("--pgcopydb=").size());
        if(a.startsWith(QStringLiteral("--pgmultidbtest=")))
            pgMultiDbArg = a.mid(QStringLiteral("--pgmultidbtest=").size());
        if(a == QStringLiteral("--dumpcombo"))
            dumpCombo = true;
        if(a.startsWith(QStringLiteral("--switchdb=")))
            switchDbArg = a.mid(QStringLiteral("--switchdb=").size());
        if(a.startsWith(QStringLiteral("--pickschema=")))
            pickSchemaArg = a.mid(QStringLiteral("--pickschema=").size());
        if(a.startsWith(QStringLiteral("--sqlitecopydb=")))
            sqliteCopyDbArg = a.mid(QStringLiteral("--sqlitecopydb=").size());
        if(a.startsWith(QStringLiteral("--sqlitecsvimport=")))
            sqliteCsvImportArg = a.mid(QStringLiteral("--sqlitecsvimport=").size());
        if(a.startsWith(QStringLiteral("--pgcsvimport=")))
            pgCsvImportArg = a.mid(QStringLiteral("--pgcsvimport=").size());
        if(a.startsWith(QStringLiteral("--schemahtmltest=")))
            schemaHtmlArg = a.mid(QStringLiteral("--schemahtmltest=").size());
        if(a.startsWith(QStringLiteral("--delconn=")))
            delConn = a.mid(QStringLiteral("--delconn=").size());
        if(a.startsWith(QStringLiteral("--shotmenu=")))
            shotMenuArg = a.mid(QStringLiteral("--shotmenu=").size());
        if(a.startsWith(QStringLiteral("--fmtsql="))) {
            QTextStream(stdout) << SqlFormat::pretty(a.mid(9)) << '\n';
            return 0;
        }
        /* the standalone selftests (qt/Selftest.h): each runs and exits */
        if(a.startsWith(QStringLiteral("--sqlitetest=")))
            return Selftest::sqliteTest(argc, argv, a.mid(QStringLiteral("--sqlitetest=").size()));
        if(a.startsWith(QStringLiteral("--sqlitecreatetabletest=")))
            return Selftest::sqliteCreateTableTest(
                argc, argv, a.mid(QStringLiteral("--sqlitecreatetabletest=").size()));
        if(a.startsWith(QStringLiteral("--sqliteindextest=")))
            return Selftest::sqliteIndexTest(argc, argv,
                                             a.mid(QStringLiteral("--sqliteindextest=").size()));
        if(a.startsWith(QStringLiteral("--sqlitemisctest=")))
            return Selftest::sqliteMiscTest(argc, argv,
                                            a.mid(QStringLiteral("--sqlitemisctest=").size()));
        if(a.startsWith(QStringLiteral("--pgtest=")))
            return Selftest::pgTest(argc, argv, a.mid(QStringLiteral("--pgtest=").size()));
        if(a.startsWith(QStringLiteral("--pgschematest=")))
            return Selftest::pgSchemaTest(argc, argv,
                                          a.mid(QStringLiteral("--pgschematest=").size()));
        if(a.startsWith(QStringLiteral("--schematest=")))
            return Selftest::schemaTest(argc, argv, a.mid(QStringLiteral("--schematest=").size()));
        if(a.startsWith(QStringLiteral("--exporttest=")))
            return Selftest::exportTest(a.mid(QStringLiteral("--exporttest=").size()));
        if(a == QStringLiteral("--shortcuttest"))
            return Selftest::shortcutTest(argc, argv);
        if(a == QStringLiteral("--stmtattest"))
            return Selftest::stmtAtTest();
        if(a == QStringLiteral("--casetest"))
            return Selftest::caseTest(argc, argv);
        if(a == QStringLiteral("--icontest"))
            return Selftest::iconTest(argc, argv);
        if(a == QStringLiteral("--filtertest"))
            return Selftest::filterTest();
        if(a == QStringLiteral("--keychaintest"))
            return Selftest::keychainTest(argc, argv);
        if(a.startsWith(QStringLiteral("--previewtheme=")))
            previewTheme = a.mid(QStringLiteral("--previewtheme=").size());
        if(a == QStringLiteral("--comptest"))
            return Selftest::compTest(argc, argv);
        if(a.startsWith(QStringLiteral("--opentable="))) {
            const QStringList parts = a.mid(12).split(':');
            if(parts.size() == 2)
                openTableParts = qMakePair(parts[0], parts[1]);
        }
        /* --dataview=text|grid : flip the Table Data pane's view toggle */
        if(a.startsWith(QStringLiteral("--dataview=")))
            dataViewMode = a.mid(QStringLiteral("--dataview=").size());
        /* --checkrows=0,2,4 : tick those rows in the row-select column */
        if(a.startsWith(QStringLiteral("--checkrows=")))
            checkRows = a.mid(QStringLiteral("--checkrows=").size());
        /* --hexcell=row:col:deadbeef : stage a binary x'…' edit and apply */
        if(a.startsWith(QStringLiteral("--hexcell=")))
            hexCell = a.mid(QStringLiteral("--hexcell=").size());
        /* --celltext=row:col : print the cell's displayed text (asserts the
         * <BLOB> placeholder for binary columns, the post-edit display, …) */
        if(a.startsWith(QStringLiteral("--celltext=")))
            cellTexts << a.mid(QStringLiteral("--celltext=").size());
        /* --tdmenu=row:col : dump the grid context menu over that cell
         * (labels + enabled — asserts the upstream IDR_QUERYLISTMENU shape) */
        if(a.startsWith(QStringLiteral("--tdmenu=")))
            tdMenus << a.mid(QStringLiteral("--tdmenu=").size());
        /* --mkobj=VIEW : open the Create <obj> editor tab */
        if(a.startsWith(QStringLiteral("--mkobj=")))
            mkObj = a.mid(QStringLiteral("--mkobj=").size());
        /* --explain=json|plain : run Explain (Format=JSON) on "SELECT 1" */
        if(a.startsWith(QStringLiteral("--explain=")))
            explainMode = a.mid(QStringLiteral("--explain=").size());
        if(a == QStringLiteral("--showinfotab"))
            showInfoTab = true;
        if(a.startsWith(QStringLiteral("--infosearch=")))
            infoSearch = a.mid(QStringLiteral("--infosearch=").size());
        if(a == QStringLiteral("--showdatatab"))
            showDataTab = true;
        if(a == QStringLiteral("--printresults"))
            printResults = true;
        if(a.startsWith(QStringLiteral("--schemasync=")))
            schemaSyncSpec = a.mid(QStringLiteral("--schemasync=").size());
        if(a.startsWith(QStringLiteral("--querybuilder=")))
            queryBuilderSpec = a.mid(QStringLiteral("--querybuilder=").size());
        if(a == QStringLiteral("--schemadesigner") ||
           a.startsWith(QStringLiteral("--schemadesigner="))) {
            schemaDesigner = true;
            schemaDesignerExport = a.mid(QStringLiteral("--schemadesigner=").size());
        }
        if(a.startsWith(QStringLiteral("--usedb=")))
            useDbArg = a.mid(QStringLiteral("--usedb=").size());
        if(a.startsWith(QStringLiteral("--expandpgdb=")))
            expandPgDbArg = a.mid(QStringLiteral("--expandpgdb=").size());
        /* --autoconnect=host:port:user:password:db */
        if(a.startsWith(QStringLiteral("--autoconnect="))) {
            const QStringList parts = a.mid(14).split(':');
            if(parts.size() == 5) {
                autoConnect.host = parts[0];
                autoConnect.port = parts[1].toInt();
                autoConnect.user = parts[2];
                autoConnect.password = parts[3];
                autoConnect.database = parts[4];
                autoConnect.name = QStringLiteral("%1@%2:%3")
                                       .arg(autoConnect.user, autoConnect.host)
                                       .arg(autoConnect.port);
                doAutoConnect = true;
            }
        }
        /* --autoconnectfile=PATH.sqlite — same downstream selftest machinery
         * (--opentable=/--editcell=/--dataview=/--checkrows=/--screenshot=/
         * --dumpdb=/--copydb=) as --autoconnect=, but for the SQLite driver */
        if(a.startsWith(QStringLiteral("--autoconnectfile="))) {
            autoConnect.driverType = SqlDriverType::Sqlite;
            autoConnect.filePath = a.mid(QStringLiteral("--autoconnectfile=").size());
            autoConnect.name = QFileInfo(autoConnect.filePath).fileName();
            doAutoConnect = true;
        }
        /* --autoconnectpg=host:port:user:password:db — same downstream
         * selftest machinery as --autoconnect=, but for the PostgreSQL driver */
        if(a.startsWith(QStringLiteral("--autoconnectpg="))) {
            const QStringList parts = a.mid(QStringLiteral("--autoconnectpg=").size()).split(':');
            if(parts.size() == 5) {
                autoConnect.driverType = SqlDriverType::Postgres;
                autoConnect.host = parts[0];
                autoConnect.port = parts[1].toInt();
                autoConnect.user = parts[2];
                autoConnect.password = parts[3];
                autoConnect.database = parts[4];
                autoConnect.name = QStringLiteral("%1@%2:%3")
                                       .arg(autoConnect.user, autoConnect.host)
                                       .arg(autoConnect.port);
                doAutoConnect = true;
            }
        }
    }

    /* headless rendering: needs to be set before QApplication starts */
    if((!screenshot.isEmpty() || !dumpPath.isEmpty() || !copyDbArg.isEmpty()) &&
       qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    /* Several Linux desktop styles (GTK among them — the platform theme
     * plugin mirrors the desktop's "gtk-menu-images" setting) default this
     * to true, which silently blanks every QAction::setIcon() in a QMenu
     * unless the action opted in with setIconVisibleInMenu(true) — nothing
     * in this app ever does, so on such a desktop the whole menu-icon pass
     * (MainWindow.cpp) would have icons in the code but none on screen.
     * Setting this attribute BEFORE the QApplication existed (this file's
     * first attempt at this fix) did not actually stick — Qt's own docs
     * for AA_DontShowIconsInMenus say to override the platform default
     * "after QGuiApplication has been instantiated", the opposite of most
     * AA_ attributes (usually set pre-construction) — so the platform
     * theme's own value was winning regardless of that earlier call. */
    app.setAttribute(Qt::AA_DontShowIconsInMenus, false);
    QApplication::setApplicationName(QStringLiteral("OpenYog"));
    QApplication::setOrganizationName(QStringLiteral("OpenYog"));
    /* saved connections, history, the last session: the user's alone */
    PrivateFiles::protectAppFolders();
    QApplication::setApplicationVersion(QStringLiteral(OPENYOG_VERSION));
    /* the installed .desktop file's name (CMakeLists.txt install rules):
     * lets a Wayland compositor match the window to its entry and icon */
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.abdulohab.openyog"));

    /* window / taskbar icon — qt/openyog.svg rasterised into qt/resources, via the .qrc */
    QIcon appIcon;
    for(int sz : {16, 32, 64, 256})
        appIcon.addFile(QStringLiteral(":/resources/openyog-%1.png").arg(sz), QSize(sz, sz));
    QApplication::setWindowIcon(appIcon);

    /* SQLyog-look tab bars; palette/stylesheets follow the saved theme */
    const QString theme = Theme::load();
    Theme::apply(app, theme);
    if(!previewTheme.isEmpty())
        Theme::apply(app, previewTheme);

    dbDriverFor(SqlDriverType::Mysql)->libraryInit();
    int rc = 0;

    /* --sshtrust: trust-on-first-use for the selftests' SSH host, the step
     * the UI asks the user about (MainWindow::openAndRun) */
    if(sshTrust && autoConnect.useSsh) {
        QString sshError;
        if(!SshTunnel::trustHostKey(autoConnect, &sshError)) {
            QTextStream(stdout) << "sshtrust: " << sshError << '\n';
            return 1;
        }
    }

    /* --delconn=NAME selftest: delete a saved connection and exit */
    if(!delConn.isEmpty()) {
        const bool existed = ConnectionStore::storedNames().contains(delConn);
        ConnectionStore::remove(delConn);
        const bool gone = !ConnectionStore::storedNames().contains(delConn);
        qInfo("delconn '%s': existed=%d removed=%d", qPrintable(delConn), existed, gone);
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return (existed && gone) ? 0 : 1;
    }

    /* --dumpdb=FILE selftest (headless, no screenshot): autoconnect, dump, exit */
    if(!dumpPath.isEmpty() && doAutoConnect) {
        MainWindow w;
        rc = (w.openAndRun(autoConnect, /*interactive=*/false) && w.selftestDump(dumpPath)) ? 0 : 1;
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --copydb=src:tgt selftest (headless): autoconnect, copy database, exit */
    if(!copyDbArg.isEmpty() && doAutoConnect) {
        const QStringList p = copyDbArg.split(':');
        MainWindow w;
        rc = (p.size() == 2 && w.openAndRun(autoConnect, /*interactive=*/false) &&
              w.selftestCopyDb(p[0], p[1]))
                 ? 0
                 : 1;
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --pgcopydb=src:tgt selftest (headless): autoconnect (via
     * --autoconnectpg=), schema-to-schema copy within the same connection, exit */
    if(!pgCopyDbArg.isEmpty() && doAutoConnect) {
        const QStringList p = pgCopyDbArg.split(':');
        MainWindow w;
        rc = (p.size() == 2 && w.openAndRun(autoConnect, /*interactive=*/false) &&
              w.selftestCopyDbPostgres(p[0], p[1]))
                 ? 0
                 : 1;
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --pgmultidbtest=otherDb selftest (headless): autoconnect (via
     * --autoconnectpg=), exercise the Object Browser's multi-database
     * side-connection plumbing (ConnectionTab::connectionFor()/paramsFor()), exit */
    if(!pgMultiDbArg.isEmpty() && doAutoConnect) {
        MainWindow w;
        rc = (w.openAndRun(autoConnect, /*interactive=*/false) && w.selftestMultiDb(pgMultiDbArg))
                 ? 0
                 : 1;
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --dumpcombo selftest (headless): autoconnect, print the toolbar
     * database combo's items, exit; --switchdb=NAME optionally switches
     * databases first (the way the tree's double-click/"Switch to `db`"
     * does) and reprints, to verify the tab stays the same (count
     * unchanged) while its title/combo update. Goes through the same QTimer +
     * QApplication::exec() shape the --screenshot= flow uses rather than
     * returning immediately after openAndRun() — connecting kicks off the
     * seed query on a worker thread, and exiting before it lands is an
     * intermittent use-after-free at shutdown (this flag does so little
     * afterward it was the first one to reliably expose that race; not
     * attempting a real fix for the race itself here, just not tripping it). */
    if(dumpCombo && doAutoConnect) {
        MainWindow w;
        bool ok = false;
        w.show(); /* lay out the real window — a synthesized click on a
                   * never-shown, zero-sized tree viewport is silently
                   * dropped by Qt (see ObjectBrowser::clickTreeItem) */
        QTimer::singleShot(1500, [&] {
            ok = w.openAndRun(autoConnect, /*interactive=*/false);
            QTimer::singleShot(600, [&] {
                QTextStream(stdout)
                    << "combo: " << w.selftestComboItems().join(QStringLiteral(", "))
                    << " tabs=" << w.selftestTabCount() << '\n';
                if(!pickSchemaArg.isEmpty()) {
                    /* setCurrentText(), not useDatabaseFromCombo() directly
                     * — the latter is the slot a real click's signal calls
                     * *after* Qt has already updated the widget's own
                     * displayed text/selection, so calling it standalone
                     * wouldn't exercise that (a real pick already showing
                     * the picked schema, not requiring a resync) */
                    w.selftestPickDropdownSchema(pickSchemaArg);
                    QTextStream(stdout)
                        << "after pick: combo=" << w.selftestComboItems().join(QStringLiteral(", "))
                        << '\n';
                }
                if(switchDbArg.isEmpty()) {
                    /* --clicktreepath works without --switchdb too: a real
                     * (synthesized) mouse click on that tree node — single-
                     * clicking a schema should make it the combo's selection
                     * right away */
                    if(!clickTreePath.isEmpty()) {
                        w.selftestClickBrowserItem(clickTreePath);
                        QTextStream(stdout)
                            << "after click " << clickTreePath
                            << ": combo=" << w.selftestComboItems().join(QStringLiteral(", "))
                            << " title=" << w.windowTitle() << '\n';
                    }
                    QApplication::quit();
                    return;
                }
                /* switching databases is the tree's job now, not the
                 * combo's (which only ever lists schemas) — this calls
                 * ConnectionTab::switchDatabase() directly, the same
                 * method a real double-click/"Switch to `db`" reaches */
                w.selftestSwitchDatabase(switchDbArg);
                QTimer::singleShot(600, [&] {
                    /* the window title encodes [tab/schema - host] — a
                     * switch-in-place changes the title's database segment
                     * but leaves tab count unchanged (still the same tab);
                     * "Connect in New Tab" would instead leave the title
                     * alone and increase the tab count */
                    QTextStream(stdout)
                        << "after switch: title=" << w.windowTitle()
                        << " combo=" << w.selftestComboItems().join(QStringLiteral(", "))
                        << " tabs=" << w.selftestTabCount() << '\n';
                    /* --clicktreepath=db/schema: a real (synthesized) mouse
                     * click on that tree node — single-clicking a schema
                     * after a switch should make it the combo's selection
                     * right away */
                    if(!clickTreePath.isEmpty()) {
                        w.selftestClickBrowserItem(clickTreePath);
                        QTextStream(stdout)
                            << "after click " << clickTreePath
                            << ": combo=" << w.selftestComboItems().join(QStringLiteral(", "))
                            << " title=" << w.windowTitle() << '\n';
                    }
                    QApplication::quit();
                });
            });
        });
        QApplication::exec();
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return ok ? 0 : 1;
    }

    /* --sqlitecopydb=target[:nodata] selftest (headless): autoconnect (via
     * --autoconnectfile=), copy the open .sqlite file, exit */
    if(!sqliteCopyDbArg.isEmpty() && doAutoConnect) {
        const bool nodata = sqliteCopyDbArg.endsWith(QStringLiteral(":nodata"));
        const QString target = nodata ? sqliteCopyDbArg.chopped(7) : sqliteCopyDbArg;
        MainWindow w;
        rc = (w.openAndRun(autoConnect, /*interactive=*/false) &&
              w.selftestCopySqliteFile(target, !nodata))
                 ? 0
                 : 1;
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --sqlitecsvimport=file.csv:table[:col,col,…] selftest (headless):
     * autoconnect (via --autoconnectfile=), import the CSV — optionally
     * into a checked subset of columns (the dialog's checklist), exit */
    if(!sqliteCsvImportArg.isEmpty() && doAutoConnect) {
        const QStringList p = sqliteCsvImportArg.split(':');
        MainWindow w;
        rc = (p.size() >= 2 && w.openAndRun(autoConnect, /*interactive=*/false) &&
              w.selftestCsvImportBatched({}, p[0], p[1], QStringLiteral("IGNORE"),
                                         p.size() > 2 ? p[2].split(QLatin1Char(','))
                                                      : QStringList()))
                 ? 0
                 : 1;
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --pgcsvimport=file.csv:table[:REPLACE] selftest (headless): autoconnect
     * (via --autoconnectpg=), import the CSV into the "public" schema, exit */
    if(!pgCsvImportArg.isEmpty() && doAutoConnect) {
        const QStringList p = pgCsvImportArg.split(':');
        MainWindow w;
        rc = (p.size() >= 2 && w.openAndRun(autoConnect, /*interactive=*/false) &&
              w.selftestCsvImportBatched(QStringLiteral("public"), p[0], p[1],
                                         p.size() > 2 ? p[2] : QStringLiteral("IGNORE"),
                                         p.size() > 3 ? p[3].split(QLatin1Char(','))
                                                      : QStringList()))
                 ? 0
                 : 1;
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --sessiontest=FILE.sqlite selftest (headless): a connection's query
     * tabs survive closing and reopening it (SessionStore) */
    if(!sessionTestArg.isEmpty()) {
        ConnectionParams p;
        p.driverType = SqlDriverType::Sqlite;
        p.filePath = sessionTestArg;
        p.name = QStringLiteral("sessiontest");
        MainWindow w;
        rc = w.selftestSessionRoundTrip(p) ? 0 : 1;
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return rc;
    }

    /* --schemahtmltest=out.html selftest (headless): autoconnect, build the
     * schema HTML, write it to disk, exit */
    if(!schemaHtmlArg.isEmpty() && doAutoConnect) {
        MainWindow w;
        rc = (w.openAndRun(autoConnect, /*interactive=*/false) &&
              w.selftestSchemaHtml(schemaHtmlArg))
                 ? 0
                 : 1;
        QThreadPool::globalInstance()->waitForDone();
        dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
        return rc;
    }

    if(!screenshot.isEmpty()) {
        if(shotDialog || shotCreateTable || shotIndexDlg || shotExportDlg || shotFkDlg ||
           shotDumpDlg) {
            QWidget *dlg = nullptr;
            if(shotExportDlg) {
                auto *ed = new ExportDialog(QStringLiteral("employees"),
                                            QStringLiteral("employees"), 42, true);
                if(auto *cb = ed->findChild<QComboBox *>())
                    cb->setCurrentText(QStringLiteral("SQL INSERT statements"));
                dlg = ed;
            } else if(shotIndexDlg) {
                IndexDialog::IndexDef pk{
                    QStringLiteral("PRIMARY"), {QStringLiteral("id")}, true, true};
                IndexDialog::IndexDef ix{
                    QStringLiteral("idx_city"), {QStringLiteral("city")}, false, false};
                dlg = new IndexDialog(QStringLiteral("port_test"), QStringLiteral("employees"),
                                      {pk, ix},
                                      {QStringLiteral("id"), QStringLiteral("name"),
                                       QStringLiteral("salary"), QStringLiteral("city")});
            } else if(shotFkDlg) {
                /* conn=nullptr: the ref-columns checklist only fills live
                 * with a connection; for the visual render the local
                 * checklist and seeded FK rows are what's being checked */
                ForeignKeyDialog::FkDef fk{QStringLiteral("fk_emp_dept"),
                                           {QStringLiteral("dept_id")},
                                           QStringLiteral("departments"),
                                           {QStringLiteral("id")}};
                dlg = new ForeignKeyDialog(
                    QStringLiteral("port_test"), QStringLiteral("employees"), {fk},
                    {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("salary"),
                     QStringLiteral("dept_id")},
                    {QStringLiteral("employees"), QStringLiteral("departments")}, nullptr);
            } else if(shotDumpDlg) {
                /* conn=nullptr renders with an empty table list — the
                 * option set itself is what's being checked visually */
                dlg = new SqlDumpDialog(nullptr, QStringLiteral("port_test"), {}, nullptr);
            } else if(shotCreateTable) {
                dlg = new CreateTableDialog(QStringLiteral("port_test"));
            } else {
                dlg = new ConnectionDialog;
                if(!dialogDriver.isEmpty()) {
                    if(auto *combo = dlg->findChild<QComboBox *>(QStringLiteral("driverCombo"))) {
                        const int idx =
                            dialogDriver.compare(QStringLiteral("sqlite"), Qt::CaseInsensitive) == 0
                                ? 1
                            : dialogDriver.compare(QStringLiteral("postgres"),
                                                   Qt::CaseInsensitive) == 0
                                ? 2
                                : 0;
                        combo->setCurrentIndex(idx);
                    }
                }
                if(!dialogTab.isEmpty())
                    if(auto *tabs =
                           dlg->findChild<QTabWidget *>(QStringLiteral("connectDialogTabs")))
                        for(int i = 0; i < tabs->count(); ++i)
                            if(tabs->tabText(i).remove(QLatin1Char('&')) == dialogTab)
                                tabs->setCurrentIndex(i);
            }
            dlg->show();
            QTimer::singleShot(200, [dlg, screenshot] {
                dlg->grab().save(screenshot);
                QApplication::quit();
            });
            QApplication::exec();
            QThreadPool::globalInstance()->waitForDone();
            dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
            return rc;
        }
        if(!shotMenuArg.isEmpty()) {
            /* find a menu by title path (mnemonic '&' stripped, case-
             * insensitive — "File", "Database>Create", "Table>Backup/Export",
             * …) and grab just that open popup, for checking menu icons/
             * labels/shortcuts without eyeballing the whole window */
            auto *w = new MainWindow;
            w->show();
            const QStringList path = shotMenuArg.split(QLatin1Char('>'));
            QMenu *target = nullptr;
            QList<QAction *> level = w->menuBar()->actions();
            for(const QString &step : path) {
                QAction *found = nullptr;
                for(QAction *a : level) {
                    QString t = a->text();
                    t.remove(QLatin1Char('&'));
                    if(t.compare(step, Qt::CaseInsensitive) == 0) {
                        found = a;
                        break;
                    }
                }
                if(!found || !found->menu()) {
                    target = nullptr;
                    break;
                }
                target = found->menu();
                level = target->actions();
            }
            if(!target) {
                QTextStream(stdout) << "shotmenu: no menu at path \"" << shotMenuArg << "\"\n";
                return 1;
            }
            target->move(60, 60);
            target->show();
            QTimer::singleShot(300, [target, screenshot] {
                target->grab().save(screenshot);
                QApplication::quit();
            });
            QApplication::exec();
            QThreadPool::globalInstance()->waitForDone();
            dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
            return rc;
        }
        std::unique_ptr<MainWindow> w(new MainWindow);
        w->show();
        /* threaded execution needs more than a frame or two to land */
        QTimer::singleShot(1500, [&, w = w.get()] {
            if(doAutoConnect)
                w->openAndRun(autoConnect, /*interactive=*/false);
            /* open the table only after the batch's results have landed,
             * so the data grid keeps the focus */
            QTimer::singleShot(600, [&, w] {
                if(!openTableParts.first.isEmpty())
                    w->openTableData(openTableParts.first, openTableParts.second);
                QTimer::singleShot(400, [&, w] {
                    if(editRow >= 0)
                        w->editTableCell(editRow, editCol, editValue, stageOnly);
                    if(!dataViewMode.isEmpty())
                        w->setDataViewMode(dataViewMode);
                    if(!checkRows.isEmpty())
                        w->setDataViewMode(QStringLiteral("check:") + checkRows);
                    if(!hexCell.isEmpty())
                        w->setDataViewMode(QStringLiteral("hex:") + hexCell);
                    for(const QString &oneCell : std::as_const(cellTexts)) {
                        const QStringList ct = oneCell.split(QLatin1Char(':'));
                        if(ct.size() == 2)
                            QTextStream(stdout)
                                << "celltext: " << w->selftestCellText(ct[0].toInt(), ct[1].toInt())
                                << '\n';
                    }
                    for(const QString &oneMenu : std::as_const(tdMenus)) {
                        const QStringList tm = oneMenu.split(QLatin1Char(':'));
                        if(tm.size() == 2)
                            QTextStream(stdout)
                                << "tdmenu:\n"
                                << w->selftestTdMenuDump(tm[0].toInt(), tm[1].toInt()) << '\n';
                    }
                    if(!useDbArg.isEmpty())
                        w->selftestUseDatabase(useDbArg);
                    if(!expandPgDbArg.isEmpty())
                        w->selftestExpandDatabase(expandPgDbArg);
                    if(!selectTreePath.isEmpty())
                        w->selftestSelectBrowserItem(selectTreePath);
                    if(!switchDbArg.isEmpty())
                        w->selftestSwitchDatabase(switchDbArg);
                    /* all four tree gestures, replayed in command-line order
                     * (single-gesture invocations are just a one-entry list —
                     * same effect as the old per-verb calls, but repeatable
                     * --xxxtreepath= args let a selftest script a whole
                     * browse sequence: expand A, double-click B, expand C…) */
                    for(const auto &act : treeActions) {
                        const QString &verb = act.first;
                        const QString &path = act.second;
                        if(verb == QStringLiteral("expand"))
                            w->selftestExpandBrowserItem(path);
                        else if(verb == QStringLiteral("collapse"))
                            w->selftestCollapseBrowserItem(path);
                        else if(verb == QStringLiteral("click"))
                            w->selftestClickBrowserItem(path);
                        else if(verb == QStringLiteral("dblclick"))
                            w->selftestDoubleClickBrowserItem(path);
                    }
                    for(const QString &dumpPath : dumpTreePaths) {
                        QTextStream(stdout) << "---- tree @ " << dumpPath << '\n';
                        for(const QString &line : w->selftestDumpTree(dumpPath))
                            QTextStream(stdout) << line << '\n';
                    }
                    for(const QString &menuPath : treeMenuPaths) {
                        QTextStream(stdout) << "---- context menu @ " << menuPath << '\n';
                        for(const QString &line : w->selftestTreeMenu(menuPath))
                            QTextStream(stdout) << line << '\n';
                    }
                    if(dumpEditor) {
                        QTextStream(stdout) << "---- editor\n";
                        for(const QString &line : w->selftestDumpEditor())
                            QTextStream(stdout) << line << '\n';
                    }
                    for(const QString &at : std::as_const(completeAts))
                        QTextStream(stdout)
                            << "completeat [" << at
                            << "]: " << w->selftestCompletions(at).join(QStringLiteral(" | "))
                            << '\n';
                    if(!editorSql.isEmpty())
                        w->selftestSetEditorText(editorSql);
                    if(showInfoTab)
                        w->selftestShowInfoTab();
                    if(!infoSearch.isEmpty())
                        QTextStream(stdout)
                            << "infosearch: " << w->selftestInfoSearch(infoSearch) << '\n';
                    if(showDataTab)
                        QTextStream(stdout) << "datatab: " << w->selftestShowDataTab() << '\n';
                    if(schemaDesigner)
                        for(const QString &line : w->selftestSchemaDesigner(schemaDesignerExport))
                            QTextStream(stdout) << line << '\n';
                    if(!schemaSyncSpec.isEmpty())
                        for(const QString &line : w->selftestSchemaSync(schemaSyncSpec))
                            QTextStream(stdout) << line << '\n';
                    if(!queryBuilderSpec.isEmpty())
                        for(const QString &line : w->selftestQueryBuilder(queryBuilderSpec))
                            QTextStream(stdout) << line << '\n';
                    /* mkObj/runAgain/explain can pop a guard QMessageBox
                     * whose modal loop blocks this callback right where the
                     * call is made — schedule the screenshot BEFORE those
                     * calls (a nested modal loop still services timers) so
                     * it fires while the dialog is up, and grab the active
                     * modal widget instead of the main window hidden behind
                     * it; quit() then unwinds every loop and the app exits.
                     * A re-run or an --explain= is also threaded — give it
                     * the same landing time as the initial connect's own
                     * query before the screenshot, instead of the other
                     * selftests' shorter delay (their effects are all
                     * synchronous). */
                    const bool threadedRun = runAgain || !explainMode.isEmpty();
                    QTimer::singleShot(threadedRun ? 1500 : 600, [w, screenshot, printResults] {
                        if(printResults)
                            for(const QString &line : w->selftestResults())
                                QTextStream(stdout) << line << '\n';
                        if(QWidget *modal = QApplication::activeModalWidget())
                            modal->grab().save(screenshot);
                        else
                            w->grab().save(screenshot);
                        QApplication::quit();
                    });
                    if(!mkObj.isEmpty())
                        w->openSchemaObjectTab(mkObj);
                    if(copyTableHost)
                        w->copySelectedTableToHost();
                    if(runAgain)
                        w->selftestRunAgain();
                    if(!explainMode.isEmpty())
                        w->selftestExplain(explainMode);
                });
            });
        });
        QApplication::exec();
    } else if(shotDialog) {
        ConnectionDialog dlg;
        dlg.exec();
    } else {
        MainWindow w;
        w.show();
        /* once the window is up: the last session's connections and tabs */
        QTimer::singleShot(0, &w, [&w] { w.restoreLastSession(); });
        rc = QApplication::exec();
    }

    QThreadPool::globalInstance()->waitForDone();
    dbDriverFor(SqlDriverType::Mysql)->libraryShutdown();
    return rc;
}
