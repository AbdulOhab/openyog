#include "ConnectionDialog.h"
#include "ConnectionStore.h"
#include "Icons.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <mysql/mysql.h>

namespace {
QWidget *placeholderTab(const QString &what)
{
    auto *w = new QWidget;
    auto *l = new QVBoxLayout(w);
    auto *lbl = new QLabel(
        QStringLiteral("%1 options arrive in a later version.").arg(what), w);
    lbl->setEnabled(false);
    l->addWidget(lbl);
    l->addStretch(1);
    return w;
}
} // namespace

ConnectionDialog::ConnectionDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Connect to MySQL Host"));
    setMinimumWidth(620);
    /* keep port / seconds fields as plain ASCII digits, like SQLyog */
    setLocale(QLocale::c());

    /* ---- left image strip (GPL bitmap from include/bitmaps) ---------- */
    auto *image = new QLabel(this);
    image->setPixmap(QPixmap(Icons::dir() + QStringLiteral("connection.png")));
    image->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    image->setFixedWidth(150);

    /* ---- New / Clone / Save / Rename / Delete ----------------------- */
    auto *newBtn  = new QPushButton(QStringLiteral("&New…"), this);
    m_clone  = new QPushButton(QStringLiteral("Clon&e…"), this);
    m_save   = new QPushButton(QStringLiteral("&Save"), this);
    m_rename = new QPushButton(QStringLiteral("&Rename…"), this);
    m_delete = new QPushButton(QStringLiteral("&Delete"), this);
    for(QPushButton *b : { m_clone, m_rename, m_delete })
        b->setEnabled(false);   /* visible-but-disabled, like SQLyog with no sel */
    connect(newBtn, &QPushButton::clicked, this, &ConnectionDialog::newConnection);
    connect(m_save, &QPushButton::clicked, this, &ConnectionDialog::saveConnection);
    auto *btnRow = new QHBoxLayout;
    for(QPushButton *b : { newBtn, m_clone, m_save, m_rename, m_delete })
        btnRow->addWidget(b);
    btnRow->addStretch(1);

    /* ---- Saved Connections combo ---------------------------------- */
    m_saved = new QComboBox(this);
    m_saved->setMinimumWidth(220);
    connect(m_saved, &QComboBox::activated, this,
            &ConnectionDialog::loadSelected);
    auto *savedLabel = new QLabel(QStringLiteral("Sa&ved Connections"), this);
    savedLabel->setBuddy(m_saved);
    auto *savedRow = new QHBoxLayout;
    savedRow->addWidget(savedLabel);
    savedRow->addWidget(m_saved, 1);

    /* ---- MySQL tab fields ---------------------------------------- */
    m_host     = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    m_user     = new QLineEdit(this);
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_savePw   = new QCheckBox(QStringLiteral("Save Pass&word"), this);
    m_savePw->setChecked(true);
    m_port     = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(3306);
    m_port->setGroupSeparatorShown(false);
    m_port->setLocale(QLocale::c());
    m_database = new QLineEdit(this);
    m_compress = new QCheckBox(QStringLiteral("Use Co&mpressed Protocol"), this);

    auto *pwRow = new QHBoxLayout;
    pwRow->addWidget(m_password, 1);
    pwRow->addWidget(m_savePw);

    auto *pwLabel = new QLabel(QStringLiteral("&Password"), this);
    pwLabel->setBuddy(m_password);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("MyS&QL Host Address"), m_host);
    form->addRow(QStringLiteral("&Username"), m_user);
    form->addRow(pwLabel, pwRow);
    form->addRow(QStringLiteral("P&ort"), m_port);
    form->addRow(QStringLiteral("Data&base(s)"), m_database);
    auto *hint = new QLabel(QStringLiteral(
        "(Use ';' to separate multiple databases. Leave blank to display all)"),
        this);
    hint->setEnabled(false);
    form->addRow(QString(), hint);
    form->addRow(QString(), m_compress);

    m_idleDefault = new QRadioButton(QStringLiteral("De&fault"), this);
    m_idleDefault->setChecked(true);
    m_idleSecs = new QSpinBox(this);
    m_idleSecs->setRange(0, 2147483);
    m_idleSecs->setValue(28800);
    m_idleSecs->setEnabled(false);
    auto *idleCustom = new QRadioButton(this);
    connect(idleCustom, &QRadioButton::toggled, m_idleSecs, &QWidget::setEnabled);
    auto *idleBox = new QGroupBox(QStringLiteral("Session Idle Timeout"), this);
    auto *idleL = new QHBoxLayout(idleBox);
    idleL->addWidget(m_idleDefault);
    idleL->addWidget(idleCustom);
    idleL->addWidget(m_idleSecs);
    idleL->addWidget(new QLabel(QStringLiteral("(seconds)"), this));
    idleL->addStretch(1);

    m_keepAlive = new QSpinBox(this);
    m_keepAlive->setRange(0, 2147483);
    auto *kaBox = new QGroupBox(QStringLiteral("&Keep-Alive Interval"), this);
    auto *kaL = new QHBoxLayout(kaBox);
    kaL->addWidget(m_keepAlive);
    kaL->addWidget(new QLabel(QStringLiteral("(seconds)"), this));
    kaL->addStretch(1);

    auto *groups = new QHBoxLayout;
    groups->addWidget(idleBox, 3);
    groups->addWidget(kaBox, 2);

    auto *mysqlTab = new QWidget(this);
    auto *mysqlLayout = new QVBoxLayout(mysqlTab);
    mysqlLayout->addLayout(form);
    mysqlLayout->addLayout(groups);
    mysqlLayout->addStretch(1);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(mysqlTab, QStringLiteral("MySQL"));
    tabs->addTab(placeholderTab(QStringLiteral("HTTP tunnel")), QStringLiteral("HTTP"));
    tabs->addTab(placeholderTab(QStringLiteral("SSH tunnel")), QStringLiteral("SSH"));
    tabs->addTab(placeholderTab(QStringLiteral("SSL")), QStringLiteral("SSL"));
    tabs->addTab(placeholderTab(QStringLiteral("Advanced")), QStringLiteral("Advanced"));

    /* ---- Connect / Cancel / Test -------------------------------- */
    auto *buttons = new QDialogButtonBox(this);
    QPushButton *connectBtn =
        buttons->addButton(QStringLiteral("&Connect"), QDialogButtonBox::AcceptRole);
    connectBtn->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    QPushButton *testBtn =
        buttons->addButton(QStringLiteral("&Test Connection"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(testBtn, &QPushButton::clicked, this, &ConnectionDialog::testConnection);

    /* ---- assemble ---------------------------------------------- */
    auto *right = new QVBoxLayout;
    right->addLayout(btnRow);
    right->addLayout(savedRow);
    right->addWidget(tabs, 1);
    right->addWidget(buttons);

    auto *top = new QHBoxLayout;
    top->addWidget(image);
    top->addLayout(right, 1);

    auto *root = new QVBoxLayout(this);
    root->addLayout(top);

    reloadSavedList();
    if(m_saved->count())
        loadSelected();
}

void ConnectionDialog::reloadSavedList()
{
    const QString keep = m_saved->currentText();
    m_saved->blockSignals(true);
    m_saved->clear();
    m_saved->addItems(ConnectionStore::storedNames());
    const int i = m_saved->findText(keep);
    m_saved->setCurrentIndex(i >= 0 ? i : 0);
    m_saved->blockSignals(false);
}

void ConnectionDialog::loadSelected()
{
    ConnectionParams p;
    if(ConnectionStore::load(m_saved->currentText(), &p))
        setParams(p);
}

void ConnectionDialog::newConnection()
{
    setParams(ConnectionParams{});
    m_host->setFocus();
    m_host->selectAll();
}

void ConnectionDialog::saveConnection()
{
    ConnectionStore::save(params());
    reloadSavedList();
    const int i = m_saved->findText(params().name);
    if(i >= 0)
        m_saved->setCurrentIndex(i);
}

void ConnectionDialog::testConnection()
{
    const ConnectionParams p = params();
    MYSQL *c = mysql_init(nullptr);
    mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    const bool ok = mysql_real_connect(
        c, p.host.toUtf8(), p.user.toUtf8(), p.password.toUtf8(),
        p.database.isEmpty() ? nullptr : p.database.toUtf8(), p.port, nullptr, 0);
    const QString msg = ok
        ? QStringLiteral("Connected to %1:%2\nServer: %3")
              .arg(p.host).arg(p.port)
              .arg(QString::fromUtf8(mysql_get_server_info(c)))
        : QStringLiteral("Connection failed:\n%1")
              .arg(QString::fromUtf8(mysql_error(c)));
    mysql_close(c);
    if(ok)
        QMessageBox::information(this, QStringLiteral("Test Connection"), msg);
    else
        QMessageBox::warning(this, QStringLiteral("Test Connection"), msg);
}

void ConnectionDialog::setParams(const ConnectionParams &p)
{
    m_host->setText(p.host);
    m_port->setValue(p.port);
    m_user->setText(p.user);
    m_password->setText(p.password);
    m_database->setText(p.database);
}

ConnectionParams ConnectionDialog::params() const
{
    ConnectionParams p;
    p.host     = m_host->text().trimmed();
    p.port     = m_port->value();
    p.user     = m_user->text().trimmed();
    p.password = m_password->text();
    p.database = m_database->text().trimmed();
    const QString sel = m_saved->currentText().trimmed();
    p.name = !sel.isEmpty() && sel != QStringLiteral("New Connection")
                 ? sel
                 : QStringLiteral("%1@%2:%3").arg(p.user, p.host).arg(p.port);
    return p;
}
