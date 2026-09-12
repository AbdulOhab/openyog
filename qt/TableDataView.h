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

class IDbConnection;

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QToolButton;

class TableDataView : public QWidget
{
    Q_OBJECT
public:
    explicit TableDataView(QWidget *parent = nullptr);

public slots:
    void load(IDbConnection *conn, const QString &db, const QString &table);
    void clear();
    void editCell(int row, int col, const QString &value);   /* selftest: stage+apply */
    void stageCellOnly(int row, int col, const QString &value); /* selftest: stage, no apply */
    void checkRowsForTest(const QString &csv);   /* selftest: tick the row-select column */
    void hexCellForTest(int row, int col, const QString &hex);  /* selftest: stage x'…' + apply */
    void setCellNull();
    void editCellInTextEditor();   /* big multi-line editor for the current cell */
    void refresh();
    void addRow();
    void insertRowWithValues();
    void applyPendingEdits();
    void revertPendingEdits();
    /* SQLyog's Grid / Form / Text view toggle (ID_VIEW_GRIDVIEW/FORMVIEW/TEXTVIEW).
     * Form view is a SQLyog Ultimate feature — the button is present but disabled,
     * mirroring Community, where it only pops the upgrade dialog. */
    void setViewMode(int mode);   /* 0 = grid, 2 = text (1 = form, unused) */

    QString loadedTable() const { return m_valid ? m_table : QString(); }
    QString loadedDb() const { return m_valid ? m_db : QString(); }

signals:
    void statusMessage(const QString &text);

private slots:
    void deleteSelectedRow();
    void duplicateRow();          /* stage a new row copied from the current one */
    void copyRows(bool withHeader);
    void exportRows();            /* write the current rows to a .csv file */
    void checkAllRows(bool on);   /* row-select checkbox column — select all / none */
    void updateApplyBar();
    void applyViewControls();      /* re-run with the current WHERE filter */
    void pageStep(int delta);
    void sortByColumn(int section);   /* header click → ORDER BY, toggles dir */

private:
    QString quoteValue(const QString &v) const;   /* quoted literal or NULL */
    /* row identity from the model's ORIGINAL values: "`pk`='v' and …" */
    QString whereFromOrigRow(int row) const;
    bool discardStagedEdits(const QString &action);  /* prompt if pending */

    void reload();
    QByteArray fetchCellBytes(int row, int col) const;   /* raw bytes for hex view */
    QString renderTextView() const;   /* column-aligned dump, upstream FormatResultSet */
    void refreshTextViewIfShown();
    /* checked rows from the row-select column (falls back to nothing);
     * copy / export / delete prefer this set over the QTableView selection */
    QList<int> checkedRows() const;

    struct ColumnInfo { QString name; bool nullable = true; bool autoInc = false; };

    IDbConnection * m_conn = nullptr;
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
    QStackedWidget * m_viewStack = nullptr;   /* grid ⇆ text */
    QTableView  * m_grid  = nullptr;
    class RowCheckHeader * m_checkHeader = nullptr;   /* left checkbox column */
    QPlainTextEdit * m_textView = nullptr;
    class TableDataModel * m_model = nullptr;

    /* toolbar laid out like SQLyog's "2 Table Data" strip:
     *   [↻ +row ⧉ 💾 ⤺ 🗑] │  … stretch …  [▽ filter] [x]Limit rows First row[n]▶ #ofrows[n]
     * sorting is by column-header click only (no combo), also like SQLyog */
    QWidget     * m_tools      = nullptr;
    QToolButton * m_tbApply    = nullptr;   /* save staged edits (enabled when pending) */
    QToolButton * m_tbRevert   = nullptr;   /* discard staged edits */
    QToolButton * m_tbDelRow   = nullptr;   /* mark current row for deletion */
    QToolButton * m_tbGrid     = nullptr;   /* view toggles — exclusive, checkable */
    QToolButton * m_tbForm     = nullptr;
    QToolButton * m_tbText     = nullptr;
    int           m_viewMode   = 0;         /* 0 = grid, 2 = text */
    QLineEdit   * m_whereEdit  = nullptr;
    QCheckBox   * m_limitChk   = nullptr;   /* off = fetch every matching row */
    QSpinBox    * m_firstRow   = nullptr;   /* 0-based OFFSET */
    QSpinBox    * m_rowCount   = nullptr;   /* LIMIT */
    QToolButton * m_nextBtn    = nullptr;   /* ▶ — advance by # of rows */
    QString       m_where;
    QString       m_orderBy;       /* "`col` ASC" or empty */
    int           m_sortColumn = -1;   /* header index currently sorted, or -1 */
    bool          m_sortDesc = false;
    long long     m_totalRows = 0;   /* COUNT(*) with the WHERE applied */
};
