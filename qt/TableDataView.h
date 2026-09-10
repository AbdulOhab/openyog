/* OpenYog — editable table-data grid (SQLyog's "2_Table Data" pane,
 * upstream DataView.cpp/CustGrid.cpp semantics):
 *   - all changes are STAGED and committed together by Apply, dropped by Revert:
 *       edited cell  → amber       (UPDATE, one per changed row)
 *       new row      → green,  ＋   (INSERT of the filled-in columns)
 *       row to delete→ red, strike, ✕  (DELETE)
 *   - Apply runs deletes → inserts → updates in one transaction, then reloads
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

class QComboBox;
class QLineEdit;
class QPushButton;
class QToolButton;

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
    void editCellInTextEditor();   /* big multi-line editor for the current cell */
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
    void applyViewControls();      /* re-run with the current WHERE / sort / page */
    void pageStep(int delta);

private:
    static QString quoteValue(const QString &v);   /* v or NULL */
    /* row identity from the model's ORIGINAL values: "`pk`='v' and …" */
    QString whereFromOrigRow(int row) const;
    bool discardStagedEdits(const QString &action);  /* prompt if pending */

    void reload();
    void rebuildSortCombo();
    QByteArray fetchCellBytes(int row, int col) const;   /* raw bytes for hex view */

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

    /* view controls: WHERE filter + ORDER BY + LIMIT/OFFSET paging */
    QWidget     * m_tools      = nullptr;
    QLineEdit   * m_whereEdit  = nullptr;
    QComboBox   * m_sortCombo  = nullptr;
    QToolButton * m_sortDirBtn = nullptr;
    QComboBox   * m_pageSize   = nullptr;
    QToolButton * m_pagePrev   = nullptr;
    QToolButton * m_pageNext   = nullptr;
    QLabel      * m_pageLabel  = nullptr;
    QString       m_where;
    QString       m_orderBy;       /* "`col` ASC" or empty */
    bool          m_sortDesc = false;
    int           m_page      = 0;
    long long     m_totalRows = 0;   /* COUNT(*) with the WHERE applied */
};
