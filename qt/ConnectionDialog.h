/* OpenYog — connection dialog, laid out after SQLyog's "Connect to MySQL Host"
 * (include/SQLyog.rc IDD_CONNECT, see xnote/2026-09-09-ui-shell-spec.md §5 and
 * xnote/ref-sqlyog-flat-connection-page.png):
 *   left image strip | New/Clone/Save/Rename/Delete row | Saved Connections
 *   combo | MySQL/HTTP/SSH/SSL/Advanced tabs | Connect / Cancel / Test.
 * HTTP/SSH/SSL/Advanced are placeholders until Phase 6. Persistence is via
 * ConnectionStore (wyIni, SQLyog-style INI). */
#pragma once

#include "ConnectionParams.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
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

private:
    void updateButtonState();

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
    QLineEdit    *m_sqlitePath  = nullptr;

    QLineEdit    *m_host     = nullptr;
    QLineEdit    *m_user     = nullptr;
    QLineEdit    *m_password = nullptr;
    QCheckBox    *m_savePw   = nullptr;
    QSpinBox     *m_port     = nullptr;
    QLineEdit    *m_database = nullptr;
    QCheckBox    *m_compress = nullptr;
    QRadioButton *m_idleDefault = nullptr;
    QSpinBox     *m_idleSecs = nullptr;
    QSpinBox     *m_keepAlive = nullptr;
};
