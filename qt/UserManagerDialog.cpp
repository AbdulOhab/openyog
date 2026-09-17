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
QString q(const QString &s)
{
    return QString(s).replace('\'', QStringLiteral("''"));
}
/* "x" with quotes doubled, for a PostgreSQL role identifier */
QString qi(const QString &s)
{
    return QLatin1Char('"') + QString(s).replace(QLatin1Char('"'), QStringLiteral("\"\"")) +
           QLatin1Char('"');
}
} // namespace

UserManagerDialog::UserManagerDialog(IDbConnection *conn, QWidget *parent, DriverType driver)
    : QDialog(parent), m_driver(driver), m_conn(conn)
{
    setWindowTitle(QStringLiteral("User Manager"));
    resize(680, 480);

    m_users = new QTableWidget(0, 3, this);
    m_users->setHorizontalHeaderLabels(
        m_driver == DriverType::Postgres
            ? QStringList{QStringLiteral("Role"), QStringLiteral("Can Login"),
                          QStringLiteral("Superuser")}
            : QStringList{QStringLiteral("User"), QStringLiteral("Host"),
                          QStringLiteral("Auth plugin")});
    m_users->horizontalHeader()->setStretchLastSection(true);
    m_users->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_users->setSelectionMode(QAbstractItemView::SingleSelection);
    m_users->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_users, &QTableWidget::itemSelectionChanged, this,
            &UserManagerDialog::showGrantsForSelection);

    auto *newBtn = new QPushButton(QStringLiteral("&New User…"), this);
    auto *dropBtn = new QPushButton(QStringLiteral("&Drop User"), this);
    auto *pwBtn = new QPushButton(QStringLiteral("Set &Password…"), this);
    auto *refresh = new QPushButton(QStringLiteral("&Refresh"), this);
    connect(newBtn, &QPushButton::clicked, this, &UserManagerDialog::createUser);
    connect(dropBtn, &QPushButton::clicked, this, &UserManagerDialog::dropUser);
    connect(pwBtn, &QPushButton::clicked, this, &UserManagerDialog::setPassword);
    connect(refresh, &QPushButton::clicked, this, &UserManagerDialog::reloadUsers);
    auto *btnRow = new QHBoxLayout;
    for(QPushButton *b : {newBtn, dropBtn, pwBtn, refresh})
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
    const bool pg = m_driver == DriverType::Postgres;
    const bool ok = m_conn->query(
        pg ? QStringLiteral("SELECT rolname, CASE WHEN rolcanlogin THEN 'yes' ELSE 'no' END, "
                            "CASE WHEN rolsuper THEN 'yes' ELSE 'no' END "
                            "FROM pg_roles ORDER BY rolname")
           : QStringLiteral("SELECT User, Host, plugin FROM mysql.user ORDER BY User, Host"),
        &rs, &error);
    if(!ok) {
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
    if(m_driver == DriverType::Postgres)
        return {m_users->item(r, 0)->text(), QString()};
    return {m_users->item(r, 0)->text(), m_users->item(r, 1)->text()};
}

void UserManagerDialog::showGrantsForSelection()
{
    const QStringList uh = selectedUserHost();
    if(uh.size() < 2)
        return;

    if(m_driver == DriverType::Postgres) {
        QString out;
        DbResultSet attrs;
        if(m_conn->query(QStringLiteral("SELECT rolsuper, rolcreatedb, rolcreaterole, rolcanlogin, "
                                        "rolreplication, rolbypassrls, rolconnlimit, "
                                        "COALESCE(rolvaliduntil::text, 'never') "
                                        "FROM pg_roles WHERE rolname = '%1'")
                             .arg(q(uh[0])),
                         &attrs, nullptr) &&
           !attrs.rows.isEmpty()) {
            const QStringList &a = attrs.rows.first();
            static const QStringList labels = {
                QStringLiteral("SUPERUSER"),   QStringLiteral("CREATEDB"),
                QStringLiteral("CREATEROLE"),  QStringLiteral("LOGIN"),
                QStringLiteral("REPLICATION"), QStringLiteral("BYPASSRLS")};
            QStringList on;
            for(int i = 0; i < labels.size(); ++i)
                if(a.value(i) == QStringLiteral("t"))
                    on << labels[i];
            out += QStringLiteral("-- role attributes\n");
            out += QStringLiteral("ALTER ROLE %1 %2;\n")
                       .arg(qi(uh[0]), on.isEmpty() ? QStringLiteral("NOLOGIN") : on.join(' '));
            out += QStringLiteral("-- connection limit: %1, password valid until: %2\n\n")
                       .arg(a.value(6), a.value(7));
        }

        DbResultSet mem;
        if(m_conn->query(QStringLiteral("SELECT r.rolname FROM pg_auth_members m "
                                        "JOIN pg_roles r ON r.oid = m.roleid "
                                        "JOIN pg_roles m2 ON m2.oid = m.member "
                                        "WHERE m2.rolname = '%1'")
                             .arg(q(uh[0])),
                         &mem, nullptr) &&
           !mem.rows.isEmpty()) {
            out += QStringLiteral("-- role membership\n");
            for(const QStringList &row : mem.rows)
                out += QStringLiteral("GRANT %1 TO %2;\n").arg(qi(row.value(0)), qi(uh[0]));
            out += QStringLiteral("\n");
        }

        DbResultSet grants;
        QString error;
        if(!m_conn->query(QStringLiteral("SELECT table_schema, table_name, privilege_type "
                                         "FROM information_schema.role_table_grants "
                                         "WHERE grantee = '%1' "
                                         "ORDER BY table_schema, table_name, privilege_type")
                              .arg(q(uh[0])),
                          &grants, &error)) {
            m_grants->setPlainText(out + error);
            return;
        }
        if(!grants.rows.isEmpty())
            out += QStringLiteral("-- table privileges\n");
        for(const QStringList &row : grants.rows)
            out += QStringLiteral("GRANT %1 ON %2.%3 TO %4;\n")
                       .arg(row.value(2), qi(row.value(0)), qi(row.value(1)), qi(uh[0]));
        m_grants->setPlainText(out);
        return;
    }

    DbResultSet rs;
    QString error;
    if(!m_conn->query(QStringLiteral("SHOW GRANTS FOR '%1'@'%2'").arg(q(uh[0]), q(uh[1])), &rs,
                      &error)) {
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
    const bool pg = m_driver == DriverType::Postgres;
    QDialog d(this);
    d.setWindowTitle(QStringLiteral("New User"));
    auto *name = new QLineEdit(&d);
    QLineEdit *host = nullptr;
    auto *pw = new QLineEdit(&d);
    pw->setEchoMode(QLineEdit::Password);
    auto *form = new QFormLayout;
    form->addRow(pg ? QStringLiteral("Role name") : QStringLiteral("User name"), name);
    if(!pg) {
        host = new QLineEdit(QStringLiteral("%"), &d);
        form->addRow(QStringLiteral("Host"), host);
    }
    form->addRow(QStringLiteral("Password"), pw);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    auto *lay = new QVBoxLayout(&d);
    lay->addLayout(form);
    lay->addWidget(bb);
    if(d.exec() != QDialog::Accepted || name->text().trimmed().isEmpty())
        return;
    const bool ok =
        pg ? run(QStringLiteral("CREATE USER %1 WITH LOGIN PASSWORD '%2'")
                     .arg(qi(name->text().trimmed()), q(pw->text())))
           : run(QStringLiteral("CREATE USER '%1'@'%2' IDENTIFIED BY '%3'")
                     .arg(q(name->text().trimmed()), q(host->text().trimmed()), q(pw->text())));
    if(ok)
        reloadUsers();
}

void UserManagerDialog::dropUser()
{
    const QStringList uh = selectedUserHost();
    if(uh.size() < 2)
        return;
    const bool pg = m_driver == DriverType::Postgres;
    if(QMessageBox::question(this, QStringLiteral("Drop User"),
                             pg ? QStringLiteral("Drop role \"%1\"?").arg(uh[0])
                                : QStringLiteral("Drop '%1'@'%2'?").arg(uh[0], uh[1])) !=
       QMessageBox::Yes)
        return;
    const bool ok = pg ? run(QStringLiteral("DROP USER %1").arg(qi(uh[0])))
                       : run(QStringLiteral("DROP USER '%1'@'%2'").arg(q(uh[0]), q(uh[1])));
    if(ok)
        reloadUsers();
}

void UserManagerDialog::setPassword()
{
    const QStringList uh = selectedUserHost();
    if(uh.size() < 2)
        return;
    const bool pg = m_driver == DriverType::Postgres;
    bool ok = false;
    const QString pw =
        QInputDialog::getText(this, QStringLiteral("Set Password"),
                              pg ? QStringLiteral("New password for \"%1\":").arg(uh[0])
                                 : QStringLiteral("New password for '%1'@'%2':").arg(uh[0], uh[1]),
                              QLineEdit::Password, QString(), &ok);
    if(!ok)
        return;
    if(pg)
        run(QStringLiteral("ALTER USER %1 WITH PASSWORD '%2'").arg(qi(uh[0]), q(pw)));
    else
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
