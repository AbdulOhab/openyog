#include "ConnectionTab.h"
#include "ObjectBrowser.h"
#include "wyString.h"

#include <QFontDatabase>
#include <QHeaderView>
#include <QPushButton>
#include <QSplitter>
#include <QTabBar>
#include <QTime>
#include <QVBoxLayout>

#include <QElapsedTimer>

#include <mysql/mysql.h>

ConnectionTab::ConnectionTab(const ConnectionParams &params, QWidget *parent)
    : QWidget(parent), m_params(params)
{
    /* ---- left: object browser ------------------------------------ */
    m_browser = new ObjectBrowser(this);

    /* ---- right-top: editor tabs (Query 1 / History) --------------- */
    m_editor = new CodeEditor(this);
    m_editor->setPlainText(QStringLiteral("SELECT VERSION(), CURRENT_USER();"));

    m_history = new QPlainTextEdit(this);
    m_history->setReadOnly(true);
    m_history->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_editorTabs = new QTabWidget(this);
    m_editorTabs->setDocumentMode(true);
    m_editorTabs->addTab(m_editor, QStringLiteral("Query 1"));
    m_editorTabs->addTab(m_history, QStringLiteral("History"));

    /* ---- right-bottom: result tabs (Messages / Result / Info) ----- */
    m_messages = new QPlainTextEdit(this);
    m_messages->setReadOnly(true);

    m_model = new QueryModel(this);
    m_grid  = new QTableView(this);
    m_grid->setModel(m_model);
    m_grid->horizontalHeader()->setStretchLastSection(true);
    m_grid->setAlternatingRowColors(true);

    m_info = new QLabel(QStringLiteral("Run a query to see server info."), this);
    m_info->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_info->setWordWrap(true);

    m_resultTabs = new QTabWidget(this);
    m_resultTabs->setDocumentMode(true);
    m_resultTabs->setTabPosition(QTabWidget::North);
    m_resultTabs->addTab(m_messages, QStringLiteral("1_Messages"));
    m_resultTabs->addTab(m_grid,     QStringLiteral("Result"));
    m_resultTabs->addTab(m_info,     QStringLiteral("3_Info"));
    m_resultTabs->setCurrentIndex(0);

    auto *rightSplit = new QSplitter(Qt::Vertical, this);
    rightSplit->addWidget(m_editorTabs);
    rightSplit->addWidget(m_resultTabs);
    rightSplit->setStretchFactor(0, 1);
    rightSplit->setStretchFactor(1, 1);

    auto *mainSplit = new QSplitter(Qt::Horizontal, this);
    mainSplit->addWidget(m_browser);
    mainSplit->addWidget(rightSplit);
    mainSplit->setStretchFactor(0, 0);
    mainSplit->setStretchFactor(1, 1);
    mainSplit->setSizes({260, 900});

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(mainSplit);

    /* ---- open the connection ------------------------------------- */
    m_conn = mysql_init(nullptr);
    mysql_options(m_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if(!mysql_real_connect(m_conn, m_params.host.toUtf8(), m_params.user.toUtf8(),
                           m_params.password.toUtf8(),
                           m_params.database.isEmpty() ? nullptr
                                                       : m_params.database.toUtf8(),
                           m_params.port, nullptr, 0)) {
        m_messages->setPlainText(QStringLiteral("Connection failed: ")
                                 + mysql_error(m_conn));
        mysql_close(m_conn);
        m_conn = nullptr;
        return;
    }

    m_browser->setConnectionLabel(
        QStringLiteral("%1@%2").arg(m_params.user, m_params.host));
    m_browser->loadDatabases(m_conn, m_params.database);
    m_messages->setPlainText(QStringLiteral(
        "Connected to %1:%2 as %3\nServer version: %4")
        .arg(m_params.host).arg(m_params.port)
        .arg(m_params.user, QString::fromUtf8(mysql_get_server_info(m_conn))));

    QStringList dbs;
    if(mysql_query(m_conn, "SHOW DATABASES") == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res))
                if(row[0])
                    dbs << QString::fromUtf8(row[0]);
            mysql_free_result(res);
        }
    }
    m_databases = dbs;
    emit databasesChanged(dbs, m_params.database);
}

ConnectionTab::~ConnectionTab()
{
    if(m_conn)
        mysql_close(m_conn);
}

void ConnectionTab::logHistory(const QString &sql)
{
    m_history->appendPlainText(
        QStringLiteral("[%1] %2")
            .arg(QTime::currentTime().toString(QStringLiteral("hh:mm:ss")), sql));
}

void ConnectionTab::runQuery()
{
    if(!m_conn) {
        m_messages->appendPlainText(QStringLiteral("not connected"));
        return;
    }

    const QString sql = m_editor->toPlainText().trimmed();
    if(sql.isEmpty())
        return;
    logHistory(sql);

    QElapsedTimer timer;
    timer.start();

    QString message;
    const bool ok = m_model->execute(m_conn, sql, &message);
    m_execSecs = timer.elapsed() / 1000.0;

    if(ok) {
        m_messages->setPlainText(message);
        emit executed(QStringLiteral("Exec: %1 sec").arg(m_execSecs, 0, 'f', 2));
        m_resultTabs->setCurrentWidget(
            m_model->rowCount() ? (QWidget *)m_grid : (QWidget *)m_messages);
    } else {
        m_messages->setPlainText(QStringLiteral("Error: ") + message);
        emit executed(QStringLiteral("Exec: %1 sec").arg(m_execSecs, 0, 'f', 2));
        m_resultTabs->setCurrentWidget(m_messages);
    }
}

void ConnectionTab::useDatabase(const QString &db)
{
    if(!m_conn)
        return;
    wyString u;
    u.Sprintf("USE `%s`", db.toUtf8().constData());
    if(mysql_query(m_conn, u.GetString()) == 0) {
        m_params.database = db;
        m_messages->setPlainText(QStringLiteral("Database changed to %1").arg(db));
        m_resultTabs->setCurrentWidget(m_messages);
    } else {
        m_messages->setPlainText(QString::fromUtf8(mysql_error(m_conn)));
    }
}
