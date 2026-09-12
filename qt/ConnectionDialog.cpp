#include "ConnectionDialog.h"
#include "ConnectionStore.h"
#include "Icons.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "db/IDbDriver.h"
#include "db/IDbConnection.h"

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
    connect(newBtn, &QPushButton::clicked, this, &ConnectionDialog::newConnection);
    connect(m_save, &QPushButton::clicked, this, &ConnectionDialog::saveConnection);
    connect(m_clone, &QPushButton::clicked, this, &ConnectionDialog::cloneConnection);
    connect(m_rename, &QPushButton::clicked, this, &ConnectionDialog::renameConnection);
    connect(m_delete, &QPushButton::clicked, this, &ConnectionDialog::deleteConnection);
    auto *btnRow = new QHBoxLayout;
    for(QPushButton *b : { newBtn, m_clone, m_save, m_rename, m_delete })
        btnRow->addWidget(b);
    btnRow->addStretch(1);

    /* ---- driver picker --------------------------------------------- */
    m_driverCombo = new QComboBox(this);
    m_driverCombo->addItem(QStringLiteral("MySQL"), QVariant::fromValue(int(DriverType::Mysql)));
    m_driverCombo->addItem(QStringLiteral("SQLite"), QVariant::fromValue(int(DriverType::Sqlite)));
    connect(m_driverCombo, &QComboBox::currentIndexChanged, this,
            &ConnectionDialog::driverChanged);
    auto *driverLabel = new QLabel(QStringLiteral("Dri&ver"), this);
    driverLabel->setBuddy(m_driverCombo);
    auto *driverRow = new QHBoxLayout;
    driverRow->addWidget(driverLabel);
    driverRow->addWidget(m_driverCombo);
    driverRow->addStretch(1);

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

    m_mysqlTab = new QWidget(this);
    auto *mysqlLayout = new QVBoxLayout(m_mysqlTab);
    mysqlLayout->addLayout(form);
    mysqlLayout->addLayout(groups);
    mysqlLayout->addStretch(1);

    /* ---- SQLite tab: just a file path + browse ---------------------- */
    m_sqliteTab = new QWidget(this);
    m_sqlitePath = new QLineEdit(m_sqliteTab);
    auto *sqliteBrowse = new QPushButton(QStringLiteral("&Browse…"), m_sqliteTab);
    connect(sqliteBrowse, &QPushButton::clicked, this,
            &ConnectionDialog::browseSqliteFile);
    auto *sqliteForm = new QFormLayout;
    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(m_sqlitePath, 1);
    pathRow->addWidget(sqliteBrowse);
    sqliteForm->addRow(QStringLiteral("&Database File"), pathRow);
    auto *sqliteLayout = new QVBoxLayout(m_sqliteTab);
    sqliteLayout->addLayout(sqliteForm);
    sqliteLayout->addStretch(1);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(m_mysqlTab, QStringLiteral("MySQL"));
    m_tabs->addTab(m_sqliteTab, QStringLiteral("SQLite"));
    m_tabs->addTab(placeholderTab(QStringLiteral("HTTP tunnel")), QStringLiteral("HTTP"));
    m_tabs->addTab(placeholderTab(QStringLiteral("SSH tunnel")), QStringLiteral("SSH"));
    m_tabs->addTab(placeholderTab(QStringLiteral("SSL")), QStringLiteral("SSL"));
    m_tabs->addTab(placeholderTab(QStringLiteral("Advanced")), QStringLiteral("Advanced"));
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        /* keep the driver combo in sync when the user clicks the SQLite tab
         * directly instead of using the combo */
        if(m_tabs->currentWidget() == m_mysqlTab)
            m_driverCombo->setCurrentIndex(0);
        else if(m_tabs->currentWidget() == m_sqliteTab)
            m_driverCombo->setCurrentIndex(1);
    });

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
    right->addLayout(driverRow);
    right->addLayout(savedRow);
    right->addWidget(m_tabs, 1);
    right->addWidget(buttons);

    auto *top = new QHBoxLayout;
    top->addWidget(image);
    top->addLayout(right, 1);

    auto *root = new QVBoxLayout(this);
    root->addLayout(top);

    reloadSavedList();
    if(m_saved->count())
        loadSelected();
    updateButtonState();
}

void ConnectionDialog::updateButtonState()
{
    const bool haveSel = m_saved->count() > 0
                         && !m_saved->currentText().trimmed().isEmpty();
    m_clone->setEnabled(haveSel);
    m_rename->setEnabled(haveSel);
    m_delete->setEnabled(haveSel);
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
    updateButtonState();
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

void ConnectionDialog::cloneConnection()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Clone Connection"),
        QStringLiteral("Name for the copy:"), QLineEdit::Normal,
        m_saved->currentText() + QStringLiteral(" (copy)"), &ok);
    if(!ok || name.trimmed().isEmpty())
        return;
    ConnectionParams p = params();
    p.name = name.trimmed();
    ConnectionStore::save(p);
    reloadSavedList();
    if(int i = m_saved->findText(p.name); i >= 0)
        m_saved->setCurrentIndex(i);
}

void ConnectionDialog::renameConnection()
{
    const QString oldName = m_saved->currentText();
    if(oldName.isEmpty())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename Connection"),
        QStringLiteral("New name:"), QLineEdit::Normal, oldName, &ok);
    if(!ok || name.trimmed().isEmpty() || name == oldName)
        return;
    if(!ConnectionStore::rename(oldName, name.trimmed())) {
        QMessageBox::warning(this, QStringLiteral("Rename Connection"),
            QStringLiteral("Could not rename '%1'.").arg(oldName));
        return;
    }
    reloadSavedList();
    if(int i = m_saved->findText(name.trimmed()); i >= 0)
        m_saved->setCurrentIndex(i);
    loadSelected();
}

void ConnectionDialog::deleteConnection()
{
    const QString name = m_saved->currentText();
    if(name.isEmpty())
        return;
    if(QMessageBox::question(this, QStringLiteral("Delete Connection"),
           QStringLiteral("Delete the saved connection '%1'?").arg(name))
           != QMessageBox::Yes)
        return;
    ConnectionStore::remove(name);
    reloadSavedList();
    if(m_saved->count())
        loadSelected();
    else
        newConnection();
}

void ConnectionDialog::testConnection()
{
    const ConnectionParams p = params();
    QString error;
    IDbConnection *c = dbDriverFor(p.driverType)->connect(p, &error);
    const bool ok = c != nullptr;
    const QString where = p.driverType == DriverType::Sqlite
        ? p.filePath : QStringLiteral("%1:%2").arg(p.host).arg(p.port);
    const QString msg = ok
        ? QStringLiteral("Connected to %1\nServer: %2").arg(where, c->serverInfo())
        : QStringLiteral("Connection failed:\n%1").arg(error);
    delete c;
    if(ok)
        QMessageBox::information(this, QStringLiteral("Test Connection"), msg);
    else
        QMessageBox::warning(this, QStringLiteral("Test Connection"), msg);
}

void ConnectionDialog::driverChanged(int index)
{
    const bool sqlite = index == 1;
    m_tabs->setCurrentWidget(sqlite ? m_sqliteTab : m_mysqlTab);
    setWindowTitle(sqlite ? QStringLiteral("Connect to SQLite Database")
                          : QStringLiteral("Connect to MySQL Host"));
}

void ConnectionDialog::browseSqliteFile()
{
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Choose or Create a SQLite Database File"),
        m_sqlitePath->text(),
        QStringLiteral("SQLite database (*.sqlite *.db *.sqlite3);;All files (*)"),
        nullptr, QFileDialog::DontConfirmOverwrite);
    if(!path.isEmpty())
        m_sqlitePath->setText(path);
}

void ConnectionDialog::setParams(const ConnectionParams &p)
{
    m_host->setText(p.host);
    m_port->setValue(p.port);
    m_user->setText(p.user);
    m_password->setText(p.password);
    m_database->setText(p.database);
    m_sqlitePath->setText(p.filePath);
    m_driverCombo->setCurrentIndex(p.driverType == DriverType::Sqlite ? 1 : 0);
}

ConnectionParams ConnectionDialog::params() const
{
    ConnectionParams p;
    p.driverType = m_tabs->currentWidget() == m_sqliteTab ? DriverType::Sqlite
                                                          : DriverType::Mysql;
    const QString sel = m_saved->currentText().trimmed();
    const bool hasSavedName = !sel.isEmpty() && sel != QStringLiteral("New Connection");

    if(p.driverType == DriverType::Sqlite) {
        p.filePath = m_sqlitePath->text().trimmed();
        p.name = hasSavedName ? sel : QFileInfo(p.filePath).baseName();
        if(p.name.isEmpty())
            p.name = QStringLiteral("New SQLite connection");
        return p;
    }

    p.host     = m_host->text().trimmed();
    p.port     = m_port->value();
    p.user     = m_user->text().trimmed();
    p.password = m_password->text();
    p.database = m_database->text().trimmed();
    p.name = hasSavedName ? sel
                          : QStringLiteral("%1@%2:%3").arg(p.user, p.host).arg(p.port);
    return p;
}
