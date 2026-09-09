/* OpenYog — Create / Alter Table dialog (Table > Create Table F4, Alter F6).
 * Column grid mirrors upstream TabFields.cpp: the prominent subset
 *   Column Name | Data Type | Length | Default | PK? | Not Null? |
 *   Unsigned? | Auto Incr? | Comment
 * The rest (charset/collation/virtuality/check) arrive later.
 * Create mode emits CREATE TABLE; Alter mode diffs the seeded columns against
 * the edited grid and emits one ALTER TABLE with ADD/CHANGE/DROP COLUMN and
 * PRIMARY KEY clauses. The dialog only builds the DDL; the caller runs it. */
#pragma once

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
    explicit CreateTableDialog(QString database, QWidget *parent = nullptr);
    /* alter mode — seed from the live table's columns */
    CreateTableDialog(QString database, QString table,
                      const QList<ColumnDef> &columns, QString engine,
                      QString charset, QWidget *parent = nullptr);

    /* CREATE TABLE … (create mode) or ALTER TABLE … (alter mode); empty when
     * there is nothing to do (no name / no columns / no changes) */
    QString buildSql() const;

private slots:
    void addColumnRow(const QString &name = {}, const QString &type = {});
    void removeSelectedRow();
    void updatePreview();

private:
    enum Col { CName, CType, CLen, CDefault, CPk, CNotNull, CUnsigned, CAuto,
               CComment, ColCount };
    enum class Mode { Create, Alter };

    void buildCommon();                 /* shared widget construction */
    void seedRow(const ColumnDef &c);   /* alter mode: row + original name tag */
    QString rowBody(int row) const;     /* "TYPE(..) UNSIGNED NOT NULL … " */
    static QString defBody(const ColumnDef &c);
    QString buildCreateSql() const;
    QString buildAlterSql() const;

    Mode         m_mode = Mode::Create;
    QString      m_database;
    QString      m_table;               /* alter mode */
    QStringList  m_originalCols;        /* names at open (alter mode) */
    QStringList  m_originalPk;          /* pk col names at open (alter mode) */
    QHash<QString, QString> m_originalBody;  /* name -> defBody() at open */

    QLineEdit   *m_name    = nullptr;
    QTableWidget*m_grid    = nullptr;
    QComboBox   *m_engine  = nullptr;
    QComboBox   *m_charset = nullptr;
    QLineEdit   *m_preview = nullptr;
};
