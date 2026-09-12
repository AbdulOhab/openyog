/* OpenYog — User Manager (Tools > User Manager, Ctrl+U).
 * Lists mysql.user, shows SHOW GRANTS for the selected account, and does the
 * common admin ops: create user, drop user, set password. Fine-grained grant
 * editing is out of scope for now — a raw GRANT/REVOKE box covers it.
 *
 * NOTE: every query here is MySQL-account-model-specific (mysql.user,
 * 'user'@'host', SHOW GRANTS) — there is no generic cross-database shape, so
 * this stays plain IDbConnection::query() text rather than new interface
 * methods. A real capability split is a Postgres-support-time problem. */
#pragma once

#include <QDialog>

class IDbConnection;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;

class UserManagerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit UserManagerDialog(IDbConnection *conn, QWidget *parent = nullptr);

private slots:
    void reloadUsers();
    void showGrantsForSelection();
    void createUser();
    void dropUser();
    void setPassword();
    void runRawGrant();

private:
    bool run(const QString &sql);            /* run + message box on error */
    QStringList selectedUserHost() const;     /* {user, host} or empty */

    IDbConnection  *m_conn = nullptr;
    QTableWidget   *m_users = nullptr;
    QPlainTextEdit *m_grants = nullptr;
    QLineEdit      *m_raw = nullptr;
};
