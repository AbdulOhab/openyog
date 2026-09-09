/* OpenYog — application entry point.
 *
 * Selftest mode for CI/headless verification:
 *   openyog --screenshot=FILE.png              render main window to FILE, exit
 *   openyog --screenshot=FILE.png --dialog     render the connection dialog
 *   openyog --screenshot=FILE.png --createtable render the Create Table dialog
 */
#include "MainWindow.h"
#include "ConnectionDialog.h"
#include "ConnectionParams.h"
#include "CreateTableDialog.h"
#include "Theme.h"

#include <QApplication>
#include <QTimer>

#include <memory>

#include <mysql/mysql.h>

int main(int argc, char *argv[])
{
    QString screenshot;
    bool shotDialog = false;
    bool shotCreateTable = false;
    QPair<QString, QString> openTableParts;
    int editRow = -1, editCol = -1;
    bool stageOnly = false;
    QString editValue;
    ConnectionParams autoConnect;
    bool doAutoConnect = false;
    QString dumpPath;
    for(int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if(a.startsWith(QStringLiteral("--screenshot=")))
            screenshot = a.mid(QStringLiteral("--screenshot=").size());
        if(a == QStringLiteral("--dialog"))
            shotDialog = true;
        if(a == QStringLiteral("--createtable"))
            shotCreateTable = true;
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
        if(a.startsWith(QStringLiteral("--opentable="))) {
            const QStringList parts = a.mid(12).split(':');
            if(parts.size() == 2)
                openTableParts = qMakePair(parts[0], parts[1]);
        }
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
    if((!screenshot.isEmpty() || !dumpPath.isEmpty())
       && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("OpenYog"));
    QApplication::setOrganizationName(QStringLiteral("OpenYog"));

    /* SQLyog-look tab bars; palette/stylesheets follow the saved theme */
    const QString theme = Theme::load();
    Theme::apply(app, theme);

    mysql_library_init(0, nullptr, nullptr);
    int rc = 0;

    /* --dumpdb=FILE selftest (headless, no screenshot): autoconnect, dump, exit */
    if(!dumpPath.isEmpty() && doAutoConnect) {
        MainWindow w;
        rc = (w.openAndRun(autoConnect) && w.selftestDump(dumpPath)) ? 0 : 1;
        mysql_library_end();
        return rc;
    }

    if(!screenshot.isEmpty()) {
        if(shotDialog || shotCreateTable) {
            QWidget *dlg = shotCreateTable
                ? static_cast<QWidget *>(
                      new CreateTableDialog(QStringLiteral("port_test")))
                : static_cast<QWidget *>(new ConnectionDialog);
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
