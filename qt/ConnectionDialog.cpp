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
#include <QFont>
#include <QPainter>
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

/* draws a plain "cylinder" database glyph — the generic icon used
 * everywhere to mean "a database", not any specific backend's mark */
void drawDbGlyph(QPainter &p, QRectF r, const QColor &bg)
{
    const qreal ellH = r.height() * 0.32;
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawRoundedRect(QRectF(r.left(), r.top() + ellH / 2, r.width(), r.height() - ellH),
                      r.width() * 0.12, r.width() * 0.12);
    p.setBrush(bg);
    p.drawEllipse(QRectF(r.left(), r.top(), r.width(), ellH));
    p.setPen(QPen(Qt::white, qMax(1.2, r.width() * 0.06)));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(r.left(), r.top(), r.width(), ellH));
}

/* The left strip's only MySQL-specific bitmap is its bottom "WORKS WITH
 * MySQL" logo band (upstream include/bitmaps/connection.png); the blue
 * background + plug icon above it are generic. For any other driver, reuse
 * that generic part and relabel the band with a plain database glyph +
 * the name — no third-party trademark reproduced, just a generic "this is
 * a database" icon, so the panel never claims a backend it isn't
 * connecting to while still looking like a deliberate badge, not an
 * afterthought. */
QPixmap genericBrandPixmap(const QString &name)
{
    const QPixmap source(Icons::dir() + QStringLiteral("connection.png"));
    const QSize size = source.isNull() ? QSize(150, 358) : source.size();
    constexpr int kIconBandHeight = 270;   /* below this = the MySQL logo band */

    QPixmap out(size);
    const QColor bg(0, 97, 138);
    out.fill(bg);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    if(!source.isNull())
        p.drawPixmap(0, 0, source, 0, 0, size.width(), kIconBandHeight);

    const int bandTop = kIconBandHeight;
    const int cx = size.width() / 2;

    p.setPen(QColor(190, 220, 232));
    QFont capFont = p.font();
    capFont.setPointSize(8);
    capFont.setBold(true);
    p.setFont(capFont);
    p.drawText(QRect(0, bandTop + 8, size.width(), 16),
               Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("WORKS WITH"));

    QFont nameFont = p.font();
    nameFont.setBold(true);
    nameFont.setPointSize(15);
    const QFontMetrics fm(nameFont);
    const int nameW = fm.horizontalAdvance(name);
    const int iconSize = 26;
    const int gap = 8;
    const int groupW = iconSize + gap + nameW;
    const int groupX = cx - groupW / 2;
    const int groupY = bandTop + 34;

    drawDbGlyph(p, QRectF(groupX, groupY, iconSize, iconSize), bg);
    p.setPen(Qt::white);
    p.setFont(nameFont);
    p.drawText(QRect(groupX + iconSize + gap, groupY - 4, nameW + 4, iconSize + 8),
               Qt::AlignVCenter | Qt::AlignLeft, name);
    p.end();
    return out;
}
} // namespace

ConnectionDialog::ConnectionDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Connect to MySQL Host"));
    setMinimumWidth(620);
    /* keep port / seconds fields as plain ASCII digits, like SQLyog */
    setLocale(QLocale::c());

    /* ---- left image strip (GPL bitmap from include/bitmaps; swaps to a
     * plain-text SQLite relabel — see sqliteBrandPixmap() — when that
     * driver is selected, so this panel never claims to be MySQL when it
     * isn't) ---------------------------------------------------------- */
    m_brandImage = new QLabel(this);
    m_brandImage->setPixmap(QPixmap(Icons::dir() + QStringLiteral("connection.png")));
    m_brandImage->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    m_brandImage->setFixedWidth(150);

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
    m_driverCombo->setObjectName(QStringLiteral("driverCombo"));   /* --dialogdriver= selftest */
    m_driverCombo->addItem(QStringLiteral("MySQL"), QVariant::fromValue(int(DriverType::Mysql)));
    m_driverCombo->addItem(QStringLiteral("SQLite"), QVariant::fromValue(int(DriverType::Sqlite)));
    m_driverCombo->addItem(QStringLiteral("PostgreSQL"),
                           QVariant::fromValue(int(DriverType::Postgres)));
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
    m_idleCustom = new QRadioButton(this);
    connect(m_idleCustom, &QRadioButton::toggled, m_idleSecs, &QWidget::setEnabled);
    auto *idleBox = new QGroupBox(QStringLiteral("Session Idle Timeout"), this);
    auto *idleL = new QHBoxLayout(idleBox);
    idleL->addWidget(m_idleDefault);
    idleL->addWidget(m_idleCustom);
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

    /* ---- PostgreSQL tab: host/port/user/password/database, no MySQL-only
     * compress/idle-timeout/keep-alive options ------------------------- */
    m_pgHost     = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    m_pgUser     = new QLineEdit(QStringLiteral("postgres"), this);
    m_pgPassword = new QLineEdit(this);
    m_pgPassword->setEchoMode(QLineEdit::Password);
    m_pgPort     = new QSpinBox(this);
    m_pgPort->setRange(1, 65535);
    m_pgPort->setValue(5432);
    m_pgPort->setGroupSeparatorShown(false);
    m_pgPort->setLocale(QLocale::c());
    m_pgDatabase = new QLineEdit(QStringLiteral("postgres"), this);
    auto *pgForm = new QFormLayout;
    pgForm->addRow(QStringLiteral("&Host Address"), m_pgHost);
    pgForm->addRow(QStringLiteral("&Username"), m_pgUser);
    pgForm->addRow(QStringLiteral("Pass&word"), m_pgPassword);
    pgForm->addRow(QStringLiteral("P&ort"), m_pgPort);
    pgForm->addRow(QStringLiteral("Data&base"), m_pgDatabase);
    auto *pgHint = new QLabel(QStringLiteral(
        "(A PostgreSQL connection is to one database — the object browser's "
        "top-level nodes are this database's schemas, not other databases)"),
        this);
    pgHint->setEnabled(false);
    pgHint->setWordWrap(true);
    pgForm->addRow(QString(), pgHint);
    m_postgresTab = new QWidget(this);
    auto *pgLayout = new QVBoxLayout(m_postgresTab);
    pgLayout->addLayout(pgForm);
    pgLayout->addStretch(1);

    /* ---- SSL tab: client-cert TLS, wired to mysql_ssl_set()/libpq ---- */
    auto *sslTab = new QWidget(this);
    m_useSsl = new QCheckBox(QStringLiteral("Use SS&L"), sslTab);
    m_sslCa   = new QLineEdit(sslTab);
    m_sslCert = new QLineEdit(sslTab);
    m_sslKey  = new QLineEdit(sslTab);
    auto *sslCaBrowse   = new QPushButton(QStringLiteral("Browse…"), sslTab);
    auto *sslCertBrowse = new QPushButton(QStringLiteral("Browse…"), sslTab);
    auto *sslKeyBrowse  = new QPushButton(QStringLiteral("Browse…"), sslTab);
    connect(sslCaBrowse, &QPushButton::clicked, this, [this] {
        browseSslFile(m_sslCa, QStringLiteral("CA Certificate")); });
    connect(sslCertBrowse, &QPushButton::clicked, this, [this] {
        browseSslFile(m_sslCert, QStringLiteral("Client Certificate")); });
    connect(sslKeyBrowse, &QPushButton::clicked, this, [this] {
        browseSslFile(m_sslKey, QStringLiteral("Client Key")); });
    auto sslRow = [](QLineEdit *edit, QPushButton *browse) {
        auto *l = new QHBoxLayout;
        l->addWidget(edit, 1);
        l->addWidget(browse);
        return l;
    };
    auto *sslForm = new QFormLayout;
    sslForm->addRow(QString(), m_useSsl);
    sslForm->addRow(QStringLiteral("CA &Certificate"), sslRow(m_sslCa, sslCaBrowse));
    sslForm->addRow(QStringLiteral("Client Cert&ificate"), sslRow(m_sslCert, sslCertBrowse));
    sslForm->addRow(QStringLiteral("Client &Key"), sslRow(m_sslKey, sslKeyBrowse));
    const auto setSslFieldsEnabled = [this](bool on) {
        m_sslCa->setEnabled(on);
        m_sslCert->setEnabled(on);
        m_sslKey->setEnabled(on);
    };
    setSslFieldsEnabled(false);
    connect(m_useSsl, &QCheckBox::toggled, this, setSslFieldsEnabled);
    auto *sslLayout = new QVBoxLayout(sslTab);
    sslLayout->addLayout(sslForm);
    sslLayout->addStretch(1);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(m_mysqlTab, QStringLiteral("MySQL"));
    m_tabs->addTab(m_sqliteTab, QStringLiteral("SQLite"));
    m_tabs->addTab(m_postgresTab, QStringLiteral("PostgreSQL"));
    m_tabs->addTab(placeholderTab(QStringLiteral("HTTP tunnel")), QStringLiteral("HTTP"));
    m_tabs->addTab(placeholderTab(QStringLiteral("SSH tunnel")), QStringLiteral("SSH"));
    m_tabs->addTab(sslTab, QStringLiteral("SSL"));
    m_tabs->addTab(placeholderTab(QStringLiteral("Advanced")), QStringLiteral("Advanced"));
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        /* keep the driver combo in sync when the user clicks a driver tab
         * directly instead of using the combo */
        if(m_tabs->currentWidget() == m_mysqlTab)
            m_driverCombo->setCurrentIndex(0);
        else if(m_tabs->currentWidget() == m_sqliteTab)
            m_driverCombo->setCurrentIndex(1);
        else if(m_tabs->currentWidget() == m_postgresTab)
            m_driverCombo->setCurrentIndex(2);
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
    top->addWidget(m_brandImage);
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
    const auto dt = DriverType(m_driverCombo->itemData(index).toInt());
    QWidget *tab = dt == DriverType::Sqlite   ? m_sqliteTab
                 : dt == DriverType::Postgres ? m_postgresTab
                                              : m_mysqlTab;
    m_tabs->setCurrentWidget(tab);
    setWindowTitle(dt == DriverType::Sqlite   ? QStringLiteral("Connect to SQLite Database")
                  : dt == DriverType::Postgres ? QStringLiteral("Connect to PostgreSQL Server")
                                               : QStringLiteral("Connect to MySQL Host"));
    m_brandImage->setPixmap(
        dt == DriverType::Sqlite   ? genericBrandPixmap(QStringLiteral("SQLite"))
      : dt == DriverType::Postgres ? genericBrandPixmap(QStringLiteral("PostgreSQL"))
                                   : QPixmap(Icons::dir() + QStringLiteral("connection.png")));
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

void ConnectionDialog::browseSslFile(QLineEdit *target, const QString &title)
{
    const QString path = QFileDialog::getOpenFileName(
        this, title, target->text(),
        QStringLiteral("PEM files (*.pem *.crt *.key);;All files (*)"));
    if(!path.isEmpty())
        target->setText(path);
}

void ConnectionDialog::setParams(const ConnectionParams &p)
{
    m_host->setText(p.host);
    m_port->setValue(p.port);
    m_user->setText(p.user);
    m_password->setText(p.password);
    m_database->setText(p.database);
    m_sqlitePath->setText(p.filePath);
    /* Postgres shares the host/port/user/password/database shape with
     * MySQL — seed its tab from the same saved values too, exactly like
     * the SQLite file path is seeded regardless of which driver a saved
     * entry actually used (harmless: switching tabs just shows whichever
     * set the user actually wants) */
    m_pgHost->setText(p.host);
    m_pgPort->setValue(p.driverType == DriverType::Postgres ? p.port : 5432);
    m_pgUser->setText(p.user);
    m_pgPassword->setText(p.password);
    m_pgDatabase->setText(p.database);
    m_driverCombo->setCurrentIndex(p.driverType == DriverType::Sqlite ? 1
                                  : p.driverType == DriverType::Postgres ? 2 : 0);
    m_useSsl->setChecked(p.useSsl);
    m_sslCa->setText(p.sslCa);
    m_sslCert->setText(p.sslCert);
    m_sslKey->setText(p.sslKey);
    m_compress->setChecked(p.compress);
    if(p.idleTimeoutSecs > 0) {
        m_idleCustom->setChecked(true);
        m_idleSecs->setValue(p.idleTimeoutSecs);
    } else {
        m_idleDefault->setChecked(true);
    }
    m_keepAlive->setValue(p.keepAliveSecs);
}

ConnectionParams ConnectionDialog::params() const
{
    ConnectionParams p;
    p.driverType = m_tabs->currentWidget() == m_sqliteTab ? DriverType::Sqlite
                 : m_tabs->currentWidget() == m_postgresTab ? DriverType::Postgres
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

    if(p.driverType == DriverType::Postgres) {
        p.host     = m_pgHost->text().trimmed();
        p.port     = m_pgPort->value();
        p.user     = m_pgUser->text().trimmed();
        p.password = m_pgPassword->text();
        p.database = m_pgDatabase->text().trimmed();
        p.useSsl   = m_useSsl->isChecked();
        p.sslCa    = m_sslCa->text().trimmed();
        p.sslCert  = m_sslCert->text().trimmed();
        p.sslKey   = m_sslKey->text().trimmed();
        p.name = hasSavedName ? sel
                              : QStringLiteral("%1@%2:%3").arg(p.user, p.host).arg(p.port);
        return p;
    }

    p.host     = m_host->text().trimmed();
    p.port     = m_port->value();
    p.user     = m_user->text().trimmed();
    p.password = m_password->text();
    p.database = m_database->text().trimmed();
    p.useSsl   = m_useSsl->isChecked();
    p.sslCa    = m_sslCa->text().trimmed();
    p.sslCert  = m_sslCert->text().trimmed();
    p.sslKey   = m_sslKey->text().trimmed();
    p.compress = m_compress->isChecked();
    p.idleTimeoutSecs = m_idleCustom->isChecked() ? m_idleSecs->value() : 0;
    p.keepAliveSecs   = m_keepAlive->value();
    p.name = hasSavedName ? sel
                          : QStringLiteral("%1@%2:%3").arg(p.user, p.host).arg(p.port);
    return p;
}
