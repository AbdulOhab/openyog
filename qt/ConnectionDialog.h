/* OpenYog — connection dialog, laid out after SQLyog's "Connect to MySQL Host"
 * (include/SQLyog.rc IDD_CONNECT):
 *   left image strip | New/Clone/Save/Rename/Delete row | Saved Connections
 *   combo | MySQL/HTTP/SSH/SSL/Advanced tabs | Connect / Cancel / Test.
 * HTTP/SSH/SSL/Advanced are placeholders until Phase 6. Persistence is via
 * ConnectionStore (wyIni, SQLyog-style INI). */
#pragma once

#include "ConnectionParams.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;

class ConnectionDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ConnectionDialog(QWidget *parent = nullptr);

    ConnectionParams params() const;

private slots:
    void reloadSavedList();
    void loadSelected();
    void newConnection();
    void saveConnection();
    void cloneConnection();
    void renameConnection();
    void deleteConnection();
    void testConnection();
    void driverChanged(int index);
    void browseSqliteFile();
    void browseSslFile(QLineEdit *target, const QString &title);

private:
    void updateButtonState();
    /* whether the current driver tab's "Save Password" box is checked
     * (SQLite has no password field, so it's always "yes, nothing to
     * hide") — read at Save/Clone time, not persisted itself: an unticked
     * box means the *next* save writes an empty password, not that the
     * connection remembers ever having been unticked */
    bool savePasswordChecked() const;

private:
    void setParams(const ConnectionParams &p);

    QComboBox    *m_saved    = nullptr;
    QPushButton  *m_clone    = nullptr;
    QPushButton  *m_save     = nullptr;
    QPushButton  *m_rename   = nullptr;
    QPushButton  *m_delete   = nullptr;

    QComboBox    *m_driverCombo = nullptr;
    QTabWidget   *m_tabs        = nullptr;
    QWidget      *m_mysqlTab    = nullptr;
    QWidget      *m_sqliteTab   = nullptr;
    QWidget      *m_postgresTab = nullptr;
    QLineEdit    *m_sqlitePath  = nullptr;

    QLineEdit    *m_pgHost     = nullptr;
    QLineEdit    *m_pgUser     = nullptr;
    QLineEdit    *m_pgPassword = nullptr;
    QCheckBox    *m_pgSavePw   = nullptr;
    QSpinBox     *m_pgPort     = nullptr;
    QLineEdit    *m_pgDatabase = nullptr;
    QRadioButton *m_pgIdleDefault = nullptr;
    QRadioButton *m_pgIdleCustom  = nullptr;
    QSpinBox     *m_pgIdleSecs    = nullptr;
    QSpinBox     *m_pgKeepAlive   = nullptr;

    QLabel       *m_brandImage  = nullptr;   /* left strip, swaps with the driver */

    QLineEdit    *m_host     = nullptr;
    QLineEdit    *m_user     = nullptr;
    QLineEdit    *m_password = nullptr;
    QCheckBox    *m_savePw   = nullptr;
    QSpinBox     *m_port     = nullptr;
    QLineEdit    *m_database = nullptr;
    QCheckBox    *m_compress = nullptr;
    QRadioButton *m_idleDefault = nullptr;
    QRadioButton *m_idleCustom = nullptr;
    QSpinBox     *m_idleSecs = nullptr;
    QSpinBox     *m_keepAlive = nullptr;

    QCheckBox    *m_useSsl  = nullptr;
    QLineEdit    *m_sslCa   = nullptr;
    QLineEdit    *m_sslCert = nullptr;
    QLineEdit    *m_sslKey  = nullptr;
};
