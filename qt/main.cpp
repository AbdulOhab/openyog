/* OpenYog — application entry point.
 *
 * Selftest mode for CI/headless verification:
 *   openyog --screenshot=FILE.png              render main window to FILE, exit
 *   openyog --screenshot=FILE.png --dialog     render the connection dialog
 *   openyog --screenshot=FILE.png --createtable render the Create Table dialog
 *   openyog --autoconnect=h:p:u:pw:db [--opentable=db:t] [--editcell=r:c:v |
 *           --stagecell=r:c:v] --screenshot=FILE.png   drive the data grid
 *   openyog --autoconnect=… --dumpdb=FILE.sql          dump a database
 *   openyog --delconn=NAME                             delete a saved connection
 *   openyog --screenshot=FILE.png --indexdlg           render the Manage Indexes dialog
 *   openyog --fmtsql="SELECT …"                        print the formatted SQL, exit
 *   openyog --comptest                                 autocomplete self-check
 */
#include "MainWindow.h"
#include "ConnectionDialog.h"
#include "ConnectionParams.h"
#include "ConnectionStore.h"
#include "CreateTableDialog.h"
#include "IndexDialog.h"
#include "SqlFormat.h"
#include "CodeEditor.h"
#include "Theme.h"

#include <QDebug>

#include <QApplication>
#include <QIcon>
#include <QTextStream>
#include <QTimer>

#include <memory>

#include <mysql/mysql.h>

int main(int argc, char *argv[])
{
    QString screenshot;
    bool shotDialog = false;
    bool shotCreateTable = false;
    bool shotIndexDlg = false;
    QPair<QString, QString> openTableParts;
    QString dataViewMode;   /* --dataview=text|grid selftest */
    QString checkRows;      /* --checkrows=0,2,4 selftest */
    QString hexCell;        /* --hexcell=row:col:hexdigits selftest */
    int editRow = -1, editCol = -1;
    bool stageOnly = false;
    QString editValue;
    ConnectionParams autoConnect;
    bool doAutoConnect = false;
    QString dumpPath;
    QString copyDbArg;
    QString delConn;
    for(int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if(a.startsWith(QStringLiteral("--screenshot=")))
            screenshot = a.mid(QStringLiteral("--screenshot=").size());
        if(a == QStringLiteral("--dialog"))
            shotDialog = true;
        if(a == QStringLiteral("--createtable"))
            shotCreateTable = true;
        if(a == QStringLiteral("--indexdlg"))
            shotIndexDlg = true;
        /* --opentable=db:table (selftest: opens the editable data grid) */
        /* --editcell=row:col:value  stage the edit AND apply it (UPDATE path) */
        /* --stagecell=row:col:value stage only (shows the amber cell + Apply bar) */
        if(a.startsWith(QStringLiteral("--editcell="))
           || a.startsWith(QStringLiteral("--stagecell="))) {
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
        if(a.startsWith(QStringLiteral("--delconn=")))
            delConn = a.mid(QStringLiteral("--delconn=").size());
        if(a.startsWith(QStringLiteral("--fmtsql="))) {
            QTextStream(stdout) << SqlFormat::pretty(a.mid(9)) << '\n';
            return 0;
        }
        if(a == QStringLiteral("--comptest")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QApplication app2(argc, argv);
            CodeEditor ed;
            ed.setCompletions({ QStringLiteral("employees"),
                                QStringLiteral("emp_dept"),
                                QStringLiteral("orders") });
            const auto countFor = [&ed](const QString &sql) {
                ed.setPlainText(sql);
                QTextCursor c = ed.textCursor();
                c.movePosition(QTextCursor::End);
                ed.setTextCursor(c);
                ed.triggerCompletion();
                return ed.completionCountForTest();
            };
            /* 1. generic fallback (no schema split) */
            const int n = countFor(QStringLiteral("SELECT emp"));
            bool ok = n >= 2;
            QTextStream(stdout) << "comptest: 'SELECT emp' = " << n
                                << (ok ? "  PASS\n" : "  FAIL\n");
            /* 2. clause-aware routing */
            ed.setCompletions({});
            ed.setSchema({ QStringLiteral("employees"), QStringLiteral("emp_dept") },
                         { QStringLiteral("emp_id"), QStringLiteral("emp_name"),
                           QStringLiteral("hire_dt") });
            const int t = countFor(QStringLiteral("SELECT * FROM emp"));
            const int col = countFor(QStringLiteral("SELECT emp"));
            const int colW = countFor(QStringLiteral("SELECT * FROM x WHERE emp"));
            const bool okT = t == 2, okC = col == 2 && colW == 2;
            QTextStream(stdout) << "comptest: FROM->tables 'emp' = " << t
                                << (okT ? "  PASS" : "  FAIL")
                                << " | SELECT/WHERE->columns 'emp' = " << col
                                << "/" << colW << (okC ? "  PASS\n" : "  FAIL\n");
            return (ok && okT && okC) ? 0 : 1;
        }
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
        /* --autoconnect=host:port:user:password:db */
        if(a.startsWith(QStringLiteral("--autoconnect="))) {
            const QStringList parts = a.mid(14).split(':');
            if(parts.size() == 5) {
                autoConnect.host = parts[0];
                autoConnect.port = parts[1].toInt();
                autoConnect.user = parts[2];
                autoConnect.password = parts[3];
                autoConnect.database = parts[4];
                autoConnect.name =
                    QStringLiteral("%1@%2:%3").arg(autoConnect.user,
                                                   autoConnect.host).arg(autoConnect.port);
                doAutoConnect = true;
            }
        }
    }

    /* headless rendering: needs to be set before QApplication starts */
    if((!screenshot.isEmpty() || !dumpPath.isEmpty() || !copyDbArg.isEmpty())
       && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("OpenYog"));
    QApplication::setOrganizationName(QStringLiteral("OpenYog"));

    /* window / taskbar icon — qt/openyog.svg rasterised into qt/resources, via the .qrc */
    QIcon appIcon;
    for(int sz : { 16, 32, 64, 256 })
        appIcon.addFile(QStringLiteral(":/resources/openyog-%1.png").arg(sz),
                        QSize(sz, sz));
    QApplication::setWindowIcon(appIcon);

    /* SQLyog-look tab bars; palette/stylesheets follow the saved theme */
    const QString theme = Theme::load();
    Theme::apply(app, theme);

    mysql_library_init(0, nullptr, nullptr);
    int rc = 0;

    /* --delconn=NAME selftest: delete a saved connection and exit */
    if(!delConn.isEmpty()) {
        const bool existed = ConnectionStore::storedNames().contains(delConn);
        ConnectionStore::remove(delConn);
        const bool gone = !ConnectionStore::storedNames().contains(delConn);
        qInfo("delconn '%s': existed=%d removed=%d",
              qPrintable(delConn), existed, gone);
        mysql_library_end();
        return (existed && gone) ? 0 : 1;
    }

    /* --dumpdb=FILE selftest (headless, no screenshot): autoconnect, dump, exit */
    if(!dumpPath.isEmpty() && doAutoConnect) {
        MainWindow w;
        rc = (w.openAndRun(autoConnect) && w.selftestDump(dumpPath)) ? 0 : 1;
        mysql_library_end();
        return rc;
    }

    /* --copydb=src:tgt selftest (headless): autoconnect, copy database, exit */
    if(!copyDbArg.isEmpty() && doAutoConnect) {
        const QStringList p = copyDbArg.split(':');
        MainWindow w;
        rc = (p.size() == 2 && w.openAndRun(autoConnect)
              && w.selftestCopyDb(p[0], p[1])) ? 0 : 1;
        mysql_library_end();
        return rc;
    }

    if(!screenshot.isEmpty()) {
        if(shotDialog || shotCreateTable || shotIndexDlg) {
            QWidget *dlg = nullptr;
            if(shotIndexDlg) {
                IndexDialog::IndexDef pk{ QStringLiteral("PRIMARY"),
                    { QStringLiteral("id") }, true, true };
                IndexDialog::IndexDef ix{ QStringLiteral("idx_city"),
                    { QStringLiteral("city") }, false, false };
                dlg = new IndexDialog(QStringLiteral("port_test"),
                    QStringLiteral("employees"), { pk, ix },
                    { QStringLiteral("id"), QStringLiteral("name"),
                      QStringLiteral("salary"), QStringLiteral("city") });
            } else if(shotCreateTable) {
                dlg = new CreateTableDialog(QStringLiteral("port_test"));
            } else {
                dlg = new ConnectionDialog;
            }
            dlg->show();
            QTimer::singleShot(200, [dlg, screenshot] {
                dlg->grab().save(screenshot);
                QApplication::quit();
            });
            QApplication::exec();
            mysql_library_end();
            return rc;
        }
        std::unique_ptr<MainWindow> w(new MainWindow);
        w->show();
        /* threaded execution needs more than a frame or two to land */
        QTimer::singleShot(1500, [&, w = w.get()] {
            if(doAutoConnect)
                w->openAndRun(autoConnect);
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
                    QTimer::singleShot(600, [w, screenshot] {
                        w->grab().save(screenshot);
                        QApplication::quit();
                    });
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
        rc = QApplication::exec();
    }

    mysql_library_end();
    return rc;
}
