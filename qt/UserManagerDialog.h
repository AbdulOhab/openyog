/* OpenYog — User Manager (Tools > User Manager, Ctrl+U).
 * Lists mysql.user, shows SHOW GRANTS for the selected account, and does the
 * common admin ops: create user, drop user, set password. Fine-grained grant
 * editing is out of scope for now — a raw GRANT/REVOKE box covers it. */
#pragma once

#include <QDialog>

#include <mysql/mysql.h>

class QLineEdit;
class QPlainTextEdit;
class QTableWidget;

class UserManagerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit UserManagerDialog(MYSQL *conn, QWidget *parent = nullptr);

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

    MYSQL          *m_conn = nullptr;
    QTableWidget   *m_users = nullptr;
    QPlainTextEdit *m_grants = nullptr;
    QLineEdit      *m_raw = nullptr;
};
