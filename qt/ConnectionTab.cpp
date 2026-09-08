#include "ConnectionTab.h"
#include "wyString.h"

#include <QFontDatabase>
#include <QHeaderView>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

#include <mysql/mysql.h>

ConnectionTab::ConnectionTab(const ConnectionParams &params, QWidget *parent)
    : QWidget(parent), m_params(params)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    m_info = new QLabel(this);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_editor = new QPlainTextEdit(this);
    m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_editor->setPlainText(QStringLiteral("SELECT VERSION(), CURRENT_USER();"));

    m_model = new QueryModel(this);
    m_grid  = new QTableView(this);
    m_grid->setModel(m_model);
    m_grid->horizontalHeader()->setStretchLastSection(true);
    m_grid->setAlternatingRowColors(true);

    auto *run = new QPushButton(QStringLiteral("&Run (Ctrl+Return)"), this);

    m_status = new QLabel(this);

    layout->addWidget(m_info);
    layout->addWidget(m_editor, 1);
    layout->addWidget(m_grid, 2);
    layout->addWidget(run);
    layout->addWidget(m_status);

    connect(run, &QPushButton::clicked, this, &ConnectionTab::runQuery);
    auto *shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), this);
    connect(shortcut, &QShortcut::activated, this, &ConnectionTab::runQuery);

    /* open the connection synchronously — one tab, one connection, like SQLyog */
    m_conn = mysql_init(nullptr);
    mysql_options(m_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if(!mysql_real_connect(m_conn, m_params.host.toUtf8(), m_params.user.toUtf8(),
                           m_params.password.toUtf8(),
                           m_params.database.isEmpty() ? nullptr
                                                       : m_params.database.toUtf8(),
                           m_params.port, nullptr, 0)) {
        m_info->setText(QStringLiteral("<b style='color:#b00'>Connection failed:</b> %1")
                            .arg(QString::fromUtf8(mysql_error(m_conn))));
        mysql_close(m_conn);
        m_conn = nullptr;
        return;
    }

    m_info->setText(QStringLiteral(
                        "<b>%1@%2:%3%4</b> &nbsp;—&nbsp; server %5")
                        .arg(m_params.user, m_params.host)
                        .arg(m_params.port)
                        .arg(m_params.database.isEmpty()
                                 ? QString()
                                 : QStringLiteral("/") + m_params.database,
                             QString::fromUtf8(mysql_get_server_info(m_conn))));
}

ConnectionTab::~ConnectionTab()
{
    if(m_conn)
        mysql_close(m_conn);
}

void ConnectionTab::runQuery()
{
    if(!m_conn) {
        m_status->setText(QStringLiteral("not connected"));
        return;
    }

    /* same core string path as the smoke test: editor text -> wyString -> server */
    wyString q;
    q.SetAs(m_editor->toPlainText().toUtf8().constData());

    QString message;
    if(m_model->execute(m_conn, q.GetString(), &message))
        m_status->setText(message);
    else
        m_status->setText(QStringLiteral("<span style='color:#b00'>%1</span>")
                              .arg(message.toHtmlEscaped()));
}
