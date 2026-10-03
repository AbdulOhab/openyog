/* OpenYog — the SQL-dump options dialog (Database/Table > Backup/Export >
 * Backup … As SQL Dump, Tools > Backup Database As SQL Dump).
 *
 * Upstream's own dialog (SQLyog.rc "SQL Dump", ExportData.cpp): object
 * tree + the IDC_CHK_* option set. Ours maps the same options onto
 * SqlDump::Options; MySQL-only rows (lock/flush) are disabled with a
 * reason on the other drivers, and "file per object" turns the target
 * into a directory with one dump file per table.
 */
#pragma once

#include <QDialog>

#include "SqlDump.h"

class IDbConnection;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QRadioButton;
class QSpinBox;

class SqlDumpDialog : public QDialog
{
    Q_OBJECT
public:
    /* `preselect` — tables to pre-check (Backup Table(s) passes its own;
     * empty checks everything, like upstream's all-checked tree) */
    SqlDumpDialog(IDbConnection *conn, const QString &db, const QStringList &preselect,
                  QWidget *parent = nullptr);

    /* checked tables; empty = all (SqlDump's own convention) */
    QStringList selectedTables() const;
    SqlDump::Options options() const;
    /* a file path — or a directory when filePerObject() is on */
    QString targetPath() const;
    bool filePerObject() const;
    /* "" or "yyyyMMdd-hhmm" — prepended to per-object file names */
    QString timestampPrefix() const;

private:
    void browse();

    IDbConnection *m_conn;
    QString m_db;
    QLineEdit *m_path = nullptr;
    QListWidget *m_tables = nullptr;
    QRadioButton *m_structOnly = nullptr;
    QRadioButton *m_dataOnly = nullptr;
    QRadioButton *m_both = nullptr;
    QCheckBox *m_drops = nullptr;
    QCheckBox *m_routines = nullptr;
    QCheckBox *m_fkOff = nullptr;
    QCheckBox *m_singleTx = nullptr;
    QCheckBox *m_lockRead = nullptr;
    QCheckBox *m_flushLogs = nullptr;
    QCheckBox *m_lockInsert = nullptr;
    QCheckBox *m_useDb = nullptr;
    QCheckBox *m_createDb = nullptr;
    QCheckBox *m_blobHex = nullptr;
    QCheckBox *m_bulk = nullptr;
    QSpinBox *m_rowsPer = nullptr;
    QCheckBox *m_filePerObject = nullptr;
    QCheckBox *m_timestamp = nullptr;
    QLabel *m_pathHint = nullptr;
};
