/* OpenYog — User Manager (Tools > User Manager, Ctrl+U).
 * Lists accounts, shows their grants, and does the common admin ops: create
 * user, drop user, set password. Fine-grained grant editing is out of scope
 * for now — a raw GRANT/REVOKE box covers it.
 *
 * MySQL: mysql.user ('user'@'host' pairs), SHOW GRANTS FOR. PostgreSQL has a
 * genuinely different account model (roles, no host component at all — a
 * connection's origin is restricted by pg_hba.conf, not per-role SQL — and
 * no single SHOW GRANTS statement), so it gets its own query set throughout:
 * pg_roles for the list and attributes, information_schema.role_table_grants
 * reformatted into GRANT-statement-shaped lines for something SHOW GRANTS-
 * like to show. Every query here is account-model-specific either way — no
 * generic cross-database shape exists for this — so this stays plain
 * IDbConnection::query() text rather than new interface methods. SQLite has
 * no account model at all; ConnectionTab guards that before ever opening
 * this dialog. */
#pragma once

#include "ConnectionParams.h"

#include <QDialog>

class IDbConnection;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;

class UserManagerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit UserManagerDialog(IDbConnection *conn, QWidget *parent = nullptr,
                               DriverType driver = DriverType::Mysql);

private slots:
    void reloadUsers();
    void showGrantsForSelection();
    void createUser();
    void dropUser();
    void setPassword();
    void runRawGrant();

private:
    bool run(const QString &sql);            /* run + message box on error */
    /* {user, host} for MySQL; {role, QString()} for PostgreSQL (kept as a
     * 2-element list either way so every call site's existing ".size() < 2"
     * empty-selection check works unchanged) — or empty when nothing's
     * selected */
    QStringList selectedUserHost() const;

    DriverType      m_driver = DriverType::Mysql;
    IDbConnection  *m_conn = nullptr;
    QTableWidget   *m_users = nullptr;
    QPlainTextEdit *m_grants = nullptr;
    QLineEdit      *m_raw = nullptr;
};
