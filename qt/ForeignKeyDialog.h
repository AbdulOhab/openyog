/* OpenYog — Relationships / Foreign Keys dialog (Table > …/Foreign Keys, F10).
 * Lists a table's FKs; Add builds one from a local column → referenced
 * table.column + ON DELETE/UPDATE actions; Remove marks one for DROP.
 * buildSql() diffs against the opened set → one ALTER TABLE with
 * DROP FOREIGN KEY / ADD CONSTRAINT … FOREIGN KEY … REFERENCES … clauses. */
#pragma once

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
                     QStringList dbTables, QWidget *parent = nullptr);

    QString buildSql() const;   /* ALTER TABLE … or empty when unchanged */

private slots:
    void addPending();
    void removeSelected();
    void updatePreview();

private:
    void addRow(const FkDef &fk, bool isNew);

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
