/* OpenYog — application entry point.
 *
 * Selftest mode for CI/headless verification:
 *   openyog --screenshot=FILE.png          render main window to FILE, exit
 *   openyog --screenshot=FILE.png --dialog render connection dialog instead
 */
#include "MainWindow.h"
#include "ConnectionDialog.h"
#include "ConnectionParams.h"

#include <QApplication>
#include <QTimer>

#include <memory>

#include <mysql/mysql.h>

int main(int argc, char *argv[])
{
    QString screenshot;
    bool shotDialog = false;
    ConnectionParams autoConnect;
    bool doAutoConnect = false;
    for(int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if(a.startsWith(QStringLiteral("--screenshot=")))
            screenshot = a.mid(QStringLiteral("--screenshot=").size());
        if(a == QStringLiteral("--dialog"))
            shotDialog = true;
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
    if(!screenshot.isEmpty() && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("OpenYog"));
    QApplication::setOrganizationName(QStringLiteral("OpenYog"));

    mysql_library_init(0, nullptr, nullptr);
    int rc = 0;

    if(!screenshot.isEmpty()) {
        if(shotDialog) {
            ConnectionDialog dlg;
            dlg.show();
            QTimer::singleShot(200, [&dlg, screenshot] {
                dlg.grab().save(screenshot);
                QApplication::quit();
            });
            QApplication::exec();
            mysql_library_end();
            return rc;
        }
        std::unique_ptr<MainWindow> w(new MainWindow);
        w->show();
        QTimer::singleShot(200, [&, w = w.get()] {
            if(doAutoConnect)
                w->openAndRun(autoConnect);
            w->grab().save(screenshot);
            QApplication::quit();
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
