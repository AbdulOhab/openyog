/* OpenYog — Create / Alter Table dialog (Table > Create Table F4, Alter F6).
 * Column grid mirrors upstream TabFields.cpp: the prominent subset
 *   Column Name | Data Type | Length | Default | PK? | Not Null? |
 *   Unsigned? | Auto Incr? | Comment
 * The rest (charset/collation/virtuality/check) arrive later.
 * Create mode emits CREATE TABLE; Alter mode diffs the seeded columns against
 * the edited grid and emits ALTER TABLE clause(s). The dialog only builds the
 * DDL; the caller runs it.
 *
 * PostgreSQL has no storage engines or per-table charset (Engine/Charset are
 * hidden for it), no unsigned integer types (Unsigned? is disabled), and no
 * AUTO_INCREMENT keyword — an Auto Incr? column instead gets "GENERATED
 * {ALWAYS|BY DEFAULT} AS IDENTITY" (portable across integer types since
 * PG 10, unlike the SERIAL pseudo-types, which are really just a spelling
 * for "integer + a sequence + a default" and don't compose as a modifier
 * the way this checkbox needs). Renames and comments are their own
 * statements in Postgres (ALTER TABLE can't RENAME COLUMN alongside other
 * clauses; there's no COMMENT clause on ALTER TABLE at all, only the
 * top-level COMMENT ON COLUMN statement) — buildAlterSql() may therefore
 * return several ';'-separated statements for Postgres, sent to the server
 * as one call: PQexec (unlike mysql_query without CLIENT_MULTI_STATEMENTS)
 * natively runs a multi-statement string as one implicit transaction. */
#pragma once

#include "ConnectionParams.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>

class QLineEdit;
class QComboBox;
class QTableWidget;

class CreateTableDialog : public QDialog
{
    Q_OBJECT
public:
    struct ColumnDef
    {
        QString name, type, length, def, comment;
        bool pk = false, notNull = false, isUnsigned = false, autoInc = false;
    };

    /* create mode */
    explicit CreateTableDialog(QString database, QWidget *parent = nullptr,
                               SqlDriverType driver = SqlDriverType::Mysql);
    /* alter mode — seed from the live table's columns */
    CreateTableDialog(QString database, QString table, const QList<ColumnDef> &columns,
                      QString engine, QString charset, QWidget *parent = nullptr,
                      SqlDriverType driver = SqlDriverType::Mysql);

    /* CREATE TABLE … (create mode) or ALTER TABLE … [;COMMENT ON …;…]
     * (alter mode, PostgreSQL only ever needs the extra statements); empty
     * when there is nothing to do (no name / no columns / no changes) */
    QString buildSql() const;
    /* alter mode, SQLite only: non-empty when buildSql() above left some
     * requested change undone because SQLite's ALTER TABLE can't express it
     * without a full table rebuild (see buildAlterSqlSqlite()) — the
     * caller should show this to the user rather than assume "no changes
     * to apply" the way an empty buildSql() with an empty limitation means */
    QString alterLimitation() const
    {
        return m_alterLimitation;
    }

private slots:
    void addColumnRow(const QString &name = {}, const QString &type = {});
    void removeSelectedRow();
    void updatePreview();

private:
    enum Col { CName, CType, CLen, CDefault, CPk, CNotNull, CUnsigned, CAuto, CComment, ColCount };
    enum class Mode { Create, Alter };

    void buildCommon();                    /* shared widget construction */
    void seedRow(const ColumnDef &c);      /* alter mode: row + original name tag */
    ColumnDef rowColumnDef(int row) const; /* current grid state of one row */
    QString rowBody(int row) const;        /* "TYPE(..) UNSIGNED NOT NULL … " */
    QString defBody(const ColumnDef &c) const;
    QString buildCreateSql() const;
    QString buildAlterSql() const;
    QString buildAlterSqlPostgres() const;
    QString buildAlterSqlSqlite() const;

    Mode m_mode = Mode::Create;
    SqlDriverType m_driver = SqlDriverType::Mysql;
    QString m_database;
    QString m_table;                          /* alter mode */
    QStringList m_originalCols;               /* names at open (alter mode) */
    QStringList m_originalPk;                 /* pk col names at open (alter mode) */
    QHash<QString, QString> m_originalBody;   /* name -> defBody() at open */
    QHash<QString, ColumnDef> m_originalDefs; /* name -> full def at open
                                               * (PostgreSQL's per-clause
                                               * ALTER needs each field,
                                               * not just the combined
                                               * body text) */
    mutable QString m_alterLimitation;        /* see alterLimitation() above */

    QLineEdit *m_name = nullptr;
    QTableWidget *m_grid = nullptr;
    QComboBox *m_engine = nullptr;
    QComboBox *m_charset = nullptr;
    QLineEdit *m_preview = nullptr;
};
