/* OpenYog — editable table-data grid (SQLyog's "2_Table Data" pane,
 * upstream DataView.cpp/CustGrid.cpp semantics):
 *   - cell edit  → UPDATE `db`.`t` SET `col`=… WHERE <row match>
 *   - row delete → DELETE FROM `db`.`t` WHERE <row match>
 *   - row match  = PK columns when a PRIMARY KEY exists (upstream
 *     IsAnyPrimary/IsColumnPrimary logic), else ALL columns (upstream fallback)
 *   - NULL cells match with IS NULL / set to NULL
 */
#pragma once

#include <QLabel>
#include <QTableView>
#include <QWidget>

#include <mysql/mysql.h>   /* typedef struct st_mysql MYSQL — no forward decl */

class TableDataView : public QWidget
{
    Q_OBJECT
public:
    explicit TableDataView(QWidget *parent = nullptr);

public slots:
    void load(MYSQL *conn, const QString &db, const QString &table);
    void clear();
    void editCell(int row, int col, const QString &value);   /* selftest */
    void setCellNull();
    void refresh();
    void addRow();
    void insertRowWithValues();

    QString loadedTable() const { return m_valid ? m_table : QString(); }

signals:
    void statusMessage(const QString &text);

private slots:
    void deleteSelectedRow();

private:
    /* row identity: "`pk1`='v' and `pk2` is null" style WHERE clause */
    QString rowMatchClause(int row) const;
    static QString quoteValue(const QString &v);   /* v or NULL */

    void reload();
    void applyCellEdit(int row, int col, const QString &oldValue,
                       const QString &newValue);

    struct ColumnInfo { QString name; bool nullable = true; bool autoInc = false; };

    MYSQL     * m_conn = nullptr;
    QString     m_db, m_table;
    QStringList m_columns;
    QList<ColumnInfo> m_colInfo;
    QList<int>  m_pkColumns;     /* indexes into m_columns */
    bool        m_hasPrimary = false;
    bool        m_valid      = false;

    QLabel      * m_label = nullptr;
    QTableView  * m_grid  = nullptr;
    class TableDataModel * m_model = nullptr;
};
