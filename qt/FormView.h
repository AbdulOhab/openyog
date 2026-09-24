/* OpenYog — Form view for the Table Data pane: one row at a time, one field
 * per column, with first/previous/next/last navigation.
 *
 * The form owns no data. It reads and writes the same model the grid uses, so
 * an edit made here is a staged edit like any other (amber, applied with the
 * Apply button, dropped by Revert), a new row is a staged insert (green) and a
 * delete is a staged delete (red). Cell values follow the grid model's own
 * convention: the literal text "NULL" means SQL NULL. */
#pragma once

#include <QAbstractItemModel>
#include <QList>
#include <QPointer>
#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QScrollArea;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

class FormView : public QWidget
{
    Q_OBJECT
public:
    struct Column
    {
        QString name;
        QString type; /* driver-native type text, shown beside the name */
        bool primary = false;
        bool nullable = true;
        bool autoInc = false;
        bool blob = false; /* shown read-only as <BLOB>; edit via the grid's Hex tab */
    };
    enum RowState { Normal = 0, Inserted = 1, Deleted = 2 };
    struct Hooks
    {
        std::function<int(int row)> rowState;        /* RowState of a model row */
        std::function<bool(int row, int col)> dirty; /* cell differs from the loaded value */
    };

    explicit FormView(QWidget *parent = nullptr);

    void setModel(QAbstractItemModel *model, const Hooks &hooks);
    /* rebuilds the field widgets when the column set changed */
    void setColumns(const QList<Column> &columns);

    int currentRow() const
    {
        return m_row;
    }
    void setCurrentRow(int row);
    /* write whatever is typed in a field into the model (edits are otherwise
     * committed when a field loses focus) */
    void commit();

    /* selftest hooks */
    void setFieldForTest(int col, const QString &text);
    QString fieldTextForTest(int col) const;
    void setNullForTest(int col, bool on);

signals:
    void newRowRequested();
    void duplicateRequested(int row);
    void deleteRequested(int row);
    void currentRowChanged(int row);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Field
    {
        QLabel *label = nullptr;
        QLineEdit *line = nullptr;
        QPlainTextEdit *multi = nullptr;
        QCheckBox *nullBox = nullptr;
        bool blank = false; /* last loaded value was NULL */
    };

    void rebuild();
    void refresh();            /* model → widgets, for m_row */
    void commitField(int col); /* widget → model */
    void updateNav();
    QString fieldText(const Field &f) const;
    void styleField(const Field &f, int col, int state);
    QString modelText(int row, int col) const;

    QAbstractItemModel *m_model = nullptr;
    Hooks m_hooks;
    QList<Column> m_columns;
    QList<Field> m_fields;
    int m_row = 0;
    bool m_loading = false; /* refresh() is writing widgets — ignore their signals */

    QToolButton *m_first = nullptr;
    QToolButton *m_prev = nullptr;
    QToolButton *m_next = nullptr;
    QToolButton *m_last = nullptr;
    QLineEdit *m_goto = nullptr; /* 1-based row number; Enter jumps */
    QLabel *m_count = nullptr;
    QToolButton *m_new = nullptr;
    QToolButton *m_dup = nullptr;
    QToolButton *m_del = nullptr;
    QLabel *m_banner = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_body = nullptr;
    QLabel *m_empty = nullptr;
};
