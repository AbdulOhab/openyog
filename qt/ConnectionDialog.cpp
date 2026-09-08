#include "ConnectionDialog.h"
#include "ConnectionStore.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

ConnectionDialog::ConnectionDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("New Connection"));
    setMinimumWidth(380);

    m_name     = new QLineEdit(this);
    m_host     = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    m_port     = new QSpinBox(this);
    m_user     = new QLineEdit(this);
    m_password = new QLineEdit(this);
    m_database = new QLineEdit(this);

    m_port->setRange(1, 65535);
    m_port->setValue(3306);
    m_password->setEchoMode(QLineEdit::Password);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("&Name"),     m_name);
    form->addRow(QStringLiteral("&Host"),     m_host);
    form->addRow(QStringLiteral("&Port"),     m_port);
    form->addRow(QStringLiteral("&User"),     m_user);
    form->addRow(QStringLiteral("&Password"), m_password);
    form->addRow(QStringLiteral("&Database"), m_database);
    form->addRow(new QLabel(QStringLiteral(
        "Saved under ~/.config/OpenYog/connections.ini (wyIni format)."), this));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    loadLastUsed();
}

void ConnectionDialog::loadLastUsed()
{
    const QStringList names = ConnectionStore::storedNames();
    if(names.isEmpty())
        return;

    ConnectionParams p;
    if(!ConnectionStore::load(names.first(), &p))
        return;

    m_name->setText(p.name);
    m_host->setText(p.host);
    m_port->setValue(p.port);
    m_user->setText(p.user);
    m_password->setText(p.password);
    m_database->setText(p.database);
}

ConnectionParams ConnectionDialog::params() const
{
    ConnectionParams p;
    p.name     = m_name->text().trimmed();
    p.host     = m_host->text().trimmed();
    p.port     = m_port->value();
    p.user     = m_user->text().trimmed();
    p.password = m_password->text();
    p.database = m_database->text().trimmed();
    if(p.name.isEmpty())
        p.name = QStringLiteral("%1@%2:%3").arg(p.user, p.host).arg(p.port);
    return p;
}
