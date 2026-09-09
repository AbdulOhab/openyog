/* OpenYog — Create Table dialog (Table > Create Table, F4).
 * Column grid mirrors upstream TabFields.cpp: the prominent subset
 *   Column Name | Data Type | Length | Default | PK? | Not Null? |
 *   Unsigned? | Auto Incr? | Comment
 * The rest (charset/collation/virtuality/check) arrive with Alter Table.
 * The dialog only builds the DDL string; the caller runs it via execDdl. */
#pragma once

#include <QDialog>

class QLineEdit;
class QComboBox;
class QTableWidget;

class CreateTableDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CreateTableDialog(QString database, QWidget *parent = nullptr);

    /* full "CREATE TABLE `db`.`name` ( … ) ENGINE=… DEFAULT CHARSET=…" or
     * empty if the form is not valid (no name / no columns) */
    QString buildSql() const;

private slots:
    void addColumnRow(const QString &name = {}, const QString &type = {});
    void removeSelectedRow();
    void updatePreview();

private:
    enum Col { CName, CType, CLen, CDefault, CPk, CNotNull, CUnsigned, CAuto,
               CComment, ColCount };

    QString      m_database;
    QLineEdit   *m_name    = nullptr;
    QTableWidget*m_grid    = nullptr;
    QComboBox   *m_engine  = nullptr;
    QComboBox   *m_charset = nullptr;
    QLineEdit   *m_preview = nullptr;
};
