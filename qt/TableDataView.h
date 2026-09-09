/* OpenYog — editable table-data grid (SQLyog's "2_Table Data" pane,
 * upstream DataView.cpp/CustGrid.cpp semantics):
 *   - cell edits are STAGED (dirty cells shown amber); Apply commits them in
 *     one transaction as one UPDATE per changed row, Revert drops them
 *   - row delete / add still act immediately (and reload, clearing staging)
 *   - row match  = PK columns when a PRIMARY KEY exists (upstream
 *     IsAnyPrimary/IsColumnPrimary logic), else ALL columns (upstream fallback);
 *     the WHERE always uses the row's ORIGINAL (pre-edit) values
 *   - NULL cells match with IS NULL / set to NULL
 */
#pragma once

#include <QLabel>
#include <QTableView>
#include <QWidget>

#include <mysql/mysql.h>   /* typedef struct st_mysql MYSQL — no forward decl */

class QPushButton;

class TableDataView : public QWidget
{
    Q_OBJECT
public:
    explicit TableDataView(QWidget *parent = nullptr);

public slots:
    void load(MYSQL *conn, const QString &db, const QString &table);
    void clear();
    void editCell(int row, int col, const QString &value);   /* selftest: stage+apply */
    void stageCellOnly(int row, int col, const QString &value); /* selftest: stage, no apply */
    void setCellNull();
    void refresh();
    void addRow();
    void insertRowWithValues();
    void applyPendingEdits();
    void revertPendingEdits();

    QString loadedTable() const { return m_valid ? m_table : QString(); }

signals:
    void statusMessage(const QString &text);

private slots:
    void deleteSelectedRow();
    void updateApplyBar();

private:
    static QString quoteValue(const QString &v);   /* v or NULL */
    /* row identity from the model's ORIGINAL values: "`pk`='v' and …" */
    QString whereFromOrigRow(int row) const;

    void reload();

    struct ColumnInfo { QString name; bool nullable = true; bool autoInc = false; };

    MYSQL     * m_conn = nullptr;
    QString     m_db, m_table;
    QStringList m_columns;
    QList<ColumnInfo> m_colInfo;
    QList<int>  m_pkColumns;     /* indexes into m_columns */
    bool        m_hasPrimary = false;
    bool        m_valid      = false;

    QLabel      * m_label = nullptr;
    QWidget     * m_applyBar = nullptr;
    QPushButton * m_applyBtn = nullptr;
    QPushButton * m_revertBtn = nullptr;
    QLabel      * m_pendingLabel = nullptr;
    QTableView  * m_grid  = nullptr;
    class TableDataModel * m_model = nullptr;
};
