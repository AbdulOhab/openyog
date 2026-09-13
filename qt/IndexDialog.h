/* OpenYog — Manage Indexes dialog (Table > Manage Indexes, F7).
 * Lists a table's indexes; Add appends a pending index, Remove marks one for
 * DROP. buildSql() diffs against the set the dialog opened with. PRIMARY is
 * shown but not editable here (use Alter Table for the PK).
 *
 * MySQL/SQLite: one ALTER TABLE with DROP INDEX / ADD [UNIQUE] INDEX clauses.
 * PostgreSQL has no such clauses at all — CREATE [UNIQUE] INDEX and DROP
 * INDEX are their own top-level statements, not something ALTER TABLE does —
 * so buildSql() there returns however many separate statements are needed,
 * ';'-joined; PQexec runs a semicolon-separated multi-statement string as
 * one implicit transaction (see CreateTableDialog.h for the same pattern). */
#pragma once

#include "ConnectionParams.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class QComboBox;
class QLineEdit;
class QListWidget;
class QCheckBox;
class QTableWidget;

class IndexDialog : public QDialog
{
    Q_OBJECT
public:
    struct IndexDef
    {
        QString     name;
        QStringList columns;
        bool        unique = false;
        bool        primary = false;
    };

    IndexDialog(QString database, QString table,
                const QList<IndexDef> &indexes, QStringList tableColumns,
                QWidget *parent = nullptr, DriverType driver = DriverType::Mysql);

    QString buildSql() const;   /* the statement(s) above, or empty when unchanged */

private slots:
    void addPending();
    void removeSelected();
    void updatePreview();

private:
    void addRow(const IndexDef &ix);

    DriverType   m_driver = DriverType::Mysql;
    QString      m_database, m_table;
    QStringList  m_columns;
    QStringList  m_originalNames;   /* index names present at open (non-PK) */

    QTableWidget *m_grid    = nullptr;
    QLineEdit    *m_newName  = nullptr;
    QListWidget  *m_newCols  = nullptr;
    QCheckBox    *m_newUnique = nullptr;
    QLineEdit    *m_preview  = nullptr;
};
