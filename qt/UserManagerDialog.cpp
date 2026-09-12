#include "UserManagerDialog.h"

#include "db/IDbConnection.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
/* 'x' with quotes doubled */
QString q(const QString &s) { return QString(s).replace('\'', QStringLiteral("''")); }
}

UserManagerDialog::UserManagerDialog(IDbConnection *conn, QWidget *parent)
    : QDialog(parent), m_conn(conn)
{
    setWindowTitle(QStringLiteral("User Manager"));
    resize(680, 480);

    m_users = new QTableWidget(0, 3, this);
    m_users->setHorizontalHeaderLabels(
        { QStringLiteral("User"), QStringLiteral("Host"),
          QStringLiteral("Auth plugin") });
    m_users->horizontalHeader()->setStretchLastSection(true);
    m_users->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_users->setSelectionMode(QAbstractItemView::SingleSelection);
    m_users->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_users, &QTableWidget::itemSelectionChanged, this,
            &UserManagerDialog::showGrantsForSelection);

    auto *newBtn  = new QPushButton(QStringLiteral("&New User…"), this);
    auto *dropBtn = new QPushButton(QStringLiteral("&Drop User"), this);
    auto *pwBtn   = new QPushButton(QStringLiteral("Set &Password…"), this);
    auto *refresh = new QPushButton(QStringLiteral("&Refresh"), this);
    connect(newBtn,  &QPushButton::clicked, this, &UserManagerDialog::createUser);
    connect(dropBtn, &QPushButton::clicked, this, &UserManagerDialog::dropUser);
    connect(pwBtn,   &QPushButton::clicked, this, &UserManagerDialog::setPassword);
    connect(refresh, &QPushButton::clicked, this, &UserManagerDialog::reloadUsers);
    auto *btnRow = new QHBoxLayout;
    for(QPushButton *b : { newBtn, dropBtn, pwBtn, refresh })
        btnRow->addWidget(b);
    btnRow->addStretch(1);

    m_grants = new QPlainTextEdit(this);
    m_grants->setReadOnly(true);

    m_raw = new QLineEdit(this);
    m_raw->setPlaceholderText(
        QStringLiteral("raw GRANT / REVOKE … (applies to the DB, then re-shows grants)"));
    auto *rawBtn = new QPushButton(QStringLiteral("&Apply"), this);
    connect(rawBtn, &QPushButton::clicked, this, &UserManagerDialog::runRawGrant);
    connect(m_raw, &QLineEdit::returnPressed, this, &UserManagerDialog::runRawGrant);
    auto *rawRow = new QHBoxLayout;
    rawRow->addWidget(m_raw, 1);
    rawRow->addWidget(rawBtn);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(m_users, 2);
    lay->addLayout(btnRow);
    lay->addWidget(new QLabel(QStringLiteral("Grants:"), this));
    lay->addWidget(m_grants, 1);
    lay->addLayout(rawRow);
    lay->addWidget(buttons);

    reloadUsers();
}

bool UserManagerDialog::run(const QString &sql)
{
    QString error;
    if(!m_conn->query(sql, nullptr, &error)) {
        QMessageBox::warning(this, QStringLiteral("User Manager"), error);
        return false;
    }
    return true;
}

void UserManagerDialog::reloadUsers()
{
    m_users->setRowCount(0);
    DbResultSet rs;
    QString error;
    if(!m_conn->query(QStringLiteral(
           "SELECT User, Host, plugin FROM mysql.user ORDER BY User, Host"),
           &rs, &error)) {
        QMessageBox::warning(this, QStringLiteral("User Manager"), error);
        return;
    }
    for(const QStringList &row : rs.rows) {
        const int r = m_users->rowCount();
        m_users->insertRow(r);
        for(int c = 0; c < 3; ++c)
            m_users->setItem(r, c, new QTableWidgetItem(row.value(c)));
    }
    m_grants->clear();
}

QStringList UserManagerDialog::selectedUserHost() const
{
    const int r = m_users->currentRow();
    if(r < 0)
        return {};
    return { m_users->item(r, 0)->text(), m_users->item(r, 1)->text() };
}

void UserManagerDialog::showGrantsForSelection()
{
    const QStringList uh = selectedUserHost();
    if(uh.size() < 2)
        return;
    DbResultSet rs;
    QString error;
    if(!m_conn->query(QStringLiteral("SHOW GRANTS FOR '%1'@'%2'")
                          .arg(q(uh[0]), q(uh[1])), &rs, &error)) {
        m_grants->setPlainText(error);
        return;
    }
    QString out;
    for(const QStringList &row : rs.rows)
        out += row.value(0) + QStringLiteral(";\n");
    m_grants->setPlainText(out);
}

void UserManagerDialog::createUser()
{
    QDialog d(this);
    d.setWindowTitle(QStringLiteral("New User"));
    auto *name = new QLineEdit(&d);
    auto *host = new QLineEdit(QStringLiteral("%"), &d);
    auto *pw   = new QLineEdit(&d);
    pw->setEchoMode(QLineEdit::Password);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("User name"), name);
    form->addRow(QStringLiteral("Host"), host);
    form->addRow(QStringLiteral("Password"), pw);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    auto *lay = new QVBoxLayout(&d);
    lay->addLayout(form);
    lay->addWidget(bb);
    if(d.exec() != QDialog::Accepted || name->text().trimmed().isEmpty())
        return;
    if(run(QStringLiteral("CREATE USER '%1'@'%2' IDENTIFIED BY '%3'")
                .arg(q(name->text().trimmed()), q(host->text().trimmed()),
                     q(pw->text()))))
        reloadUsers();
}

void UserManagerDialog::dropUser()
{
    const QStringList uh = selectedUserHost();
    if(uh.size() < 2)
        return;
    if(QMessageBox::question(this, QStringLiteral("Drop User"),
           QStringLiteral("Drop '%1'@'%2'?").arg(uh[0], uh[1])) != QMessageBox::Yes)
        return;
    if(run(QStringLiteral("DROP USER '%1'@'%2'").arg(q(uh[0]), q(uh[1]))))
        reloadUsers();
}

void UserManagerDialog::setPassword()
{
    const QStringList uh = selectedUserHost();
    if(uh.size() < 2)
        return;
    bool ok = false;
    const QString pw = QInputDialog::getText(
        this, QStringLiteral("Set Password"),
        QStringLiteral("New password for '%1'@'%2':").arg(uh[0], uh[1]),
        QLineEdit::Password, QString(), &ok);
    if(!ok)
        return;
    run(QStringLiteral("ALTER USER '%1'@'%2' IDENTIFIED BY '%3'")
             .arg(q(uh[0]), q(uh[1]), q(pw)));
}

void UserManagerDialog::runRawGrant()
{
    const QString sql = m_raw->text().trimmed();
    if(sql.isEmpty())
        return;
    if(run(sql)) {
        m_raw->clear();
        showGrantsForSelection();
    }
}
