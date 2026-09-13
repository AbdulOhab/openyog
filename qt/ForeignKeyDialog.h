/* OpenYog — Relationships / Foreign Keys dialog (Table > …/Foreign Keys, F10).
 * Lists a table's FKs; Add builds one from a local column → referenced
 * table.column + ON DELETE/UPDATE actions; Remove marks one for DROP.
 * buildSql() diffs against the opened set → one ALTER TABLE with DROP …/
 * ADD CONSTRAINT … FOREIGN KEY … REFERENCES … clauses — "ADD CONSTRAINT …
 * FOREIGN KEY" is standard SQL and needs no PostgreSQL branch at all; only
 * the DROP side differs (MySQL's DROP FOREIGN KEY name vs standard SQL's
 * DROP CONSTRAINT name, which Postgres uses since a foreign key there is
 * just a constraint like any other) and identifier quoting. */
#pragma once

#include "ConnectionParams.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class QComboBox;
class QLineEdit;
class QTableWidget;

class ForeignKeyDialog : public QDialog
{
    Q_OBJECT
public:
    struct FkDef
    {
        QString     name;
        QStringList columns;
        QString     refTable;
        QStringList refColumns;
        QString     onDelete = QStringLiteral("RESTRICT");
        QString     onUpdate = QStringLiteral("RESTRICT");
    };

    ForeignKeyDialog(QString database, QString table,
                     const QList<FkDef> &fks, QStringList tableColumns,
                     QStringList dbTables, QWidget *parent = nullptr,
                     DriverType driver = DriverType::Mysql);

    QString buildSql() const;   /* ALTER TABLE … or empty when unchanged */

private slots:
    void addPending();
    void removeSelected();
    void updatePreview();

private:
    void addRow(const FkDef &fk, bool isNew);

    DriverType   m_driver = DriverType::Mysql;
    QString      m_database, m_table;
    QStringList  m_columns, m_dbTables;
    QStringList  m_originalNames;

    QTableWidget *m_grid = nullptr;
    QLineEdit    *m_name = nullptr;
    QComboBox    *m_localCol = nullptr;
    QComboBox    *m_refTable = nullptr;
    QLineEdit    *m_refCol = nullptr;
    QComboBox    *m_onDelete = nullptr;
    QComboBox    *m_onUpdate = nullptr;
    QLineEdit    *m_preview = nullptr;
};
