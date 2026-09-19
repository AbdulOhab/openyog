/* OpenYog — Relationships / Foreign Keys dialog (Table > …/Foreign Keys, F10).
 * Lists a table's FKs; Add builds one from a local column → referenced
 * table.column + ON DELETE/UPDATE actions; Remove marks one for DROP.
 * buildSql() diffs against the opened set → one ALTER TABLE with DROP …/
 * ADD CONSTRAINT … FOREIGN KEY … REFERENCES … clauses — "ADD CONSTRAINT …
 * FOREIGN KEY" is standard SQL and needs no PostgreSQL branch at all; only
 * the DROP side differs (MySQL's DROP FOREIGN KEY name vs standard SQL's
 * DROP CONSTRAINT name, which Postgres uses since a foreign key there is
 * just a constraint like any other) and identifier quoting.
 *
 * SQLite is a real exception, not just a quoting difference: its ALTER
 * TABLE can't add OR drop a foreign key constraint on an existing table at
 * all (both need the classic create-new/copy-data/drop-old/rename table
 * rebuild, same limitation as CreateTableDialog's SQLite Alter path) — so
 * for SQLite, buildSql() always returns empty and limitation() explains why,
 * rather than emitting ALTER TABLE ADD/DROP CONSTRAINT that would just fail.
 * The dialog still opens and lists existing FKs (read-only) — SQLite's
 * `listForeignKeys()` works fine, only the ADD/DROP TABLE surgery doesn't. */
#pragma once

#include "ConnectionParams.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class IDbConnection;
class QComboBox;
class QLineEdit;
class QListWidget;
class QTableWidget;

class ForeignKeyDialog : public QDialog
{
    Q_OBJECT
public:
    struct FkDef
    {
        QString name;
        QStringList columns;
        QString refTable;
        QStringList refColumns;
        QString onDelete = QStringLiteral("RESTRICT");
        QString onUpdate = QStringLiteral("RESTRICT");
    };

    /* `conn` is used only to fetch the REFERENCED table's columns live, as
     * the user picks a different one in m_refTable — this table's own
     * columns (tableColumns) are already known, but a foreign key can
     * point at any other table, whose columns aren't fetched up front. */
    ForeignKeyDialog(QString database, QString table, const QList<FkDef> &fks,
                     QStringList tableColumns, QStringList dbTables, IDbConnection *conn,
                     QWidget *parent = nullptr, SqlDriverType driver = SqlDriverType::Mysql);

    QString buildSql() const; /* ALTER TABLE … or empty when unchanged */
    /* SQLite only: non-empty when buildSql() left a requested add/drop
     * undone because SQLite can't express it without a full table rebuild */
    QString limitation() const
    {
        return m_limitation;
    }

private slots:
    void addPending();
    void removeSelected();
    void updatePreview();
    void reloadRefColumns(const QString &refTable);

private:
    void addRow(const FkDef &fk, bool isNew);

    mutable QString m_limitation;
    SqlDriverType m_driver = SqlDriverType::Mysql;
    QString m_database, m_table;
    QStringList m_columns, m_dbTables;
    QStringList m_originalNames;
    IDbConnection *m_conn = nullptr;

    QTableWidget *m_grid = nullptr;
    QLineEdit *m_name = nullptr;
    QListWidget *m_localCols = nullptr;
    QComboBox *m_refTable = nullptr;
    QListWidget *m_refCols = nullptr;
    QComboBox *m_onDelete = nullptr;
    QComboBox *m_onUpdate = nullptr;
    QLineEdit *m_preview = nullptr;
};
