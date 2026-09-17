/* OpenYog — Table Data's "Custom Filter" dialog, opened from the funnel
 * toolbar button. Ports upstream src/SortAndFilter.cpp's IDD_CUSTOMFILTER
 * (server-side mode, m_filtercolumns == 5) faithfully rather than
 * reimplementing from scratch: up to 5 Field/Condition/Value rows, a blank
 * Field means "skip this row", every filled row is AND-joined (upstream
 * has no OR), "=" / "<>" against a value of NULL or (NULL) becomes
 * IS [NOT] NULL (SetFilterString()), and LIKE auto-detects a leading
 * and/or trailing '%' typed into Value to mean "ends with" / "starts
 * with" / "contains" (ProcessFilter()'s FT_LIKEBEGIN/END/BOTH split) —
 * same rules, same field order, same "Show/Hide SQL Preview" toggle. */
#pragma once

#include <QDialog>
#include <QStringList>
#include <QVector>

#include <functional>

class QCheckBox;
class QComboBox;
class QLineEdit;

class CustomFilterDialog : public QDialog
{
    Q_OBJECT
public:
    struct Row
    {
        QString field;
        QString condition;
        QString value;
    };
    static constexpr int kRows = 5;

    /* `quoteIdent`/`escapeValue` mirror IDbConnection::quoteIdent()/
     * escape() exactly — passed in so the dialog's own live SQL preview
     * matches, character for character, whatever the caller ends up
     * running, without this dialog needing to know about IDbConnection
     * (or drivers) at all. `escapeValue` returns the escaped text with NO
     * surrounding quotes — this dialog adds those itself (needed to place
     * '%' correctly inside them for LIKE). */
    /* `fullQueryFor`: given the WHERE clause the current rows build to
     * (may be empty), returns the complete query that would actually run
     * if OK were pressed right now — upstream's own SQL Preview shows the
     * full SELECT (FROM/ORDER BY/LIMIT and all), not just the WHERE
     * fragment (src/SortAndFilter.cpp's IQueryBuilder::GetQuery()). Optional:
     * omitted (or left null), the preview falls back to just "WHERE …". */
    CustomFilterDialog(const QStringList &columns, const QVector<Row> &initial,
                       std::function<QString(const QString &)> quoteIdent,
                       std::function<QString(const QString &)> escapeValue,
                       std::function<QString(const QString &)> fullQueryFor = {},
                       QWidget *parent = nullptr);

    /* every row as currently filled in (blank Field included) — round-trip
     * this back in as `initial` next time to reopen with the same rows,
     * upstream's EndFilter() "copy current filter back" behavior */
    QVector<Row> rows() const;
    /* the WHERE clause the current rows build to (empty if every row's
     * Field is blank) */
    QString whereClause() const;

    /* the actual row → SQL fragment logic, exposed standalone so a caller
     * needing to rebuild the same WHERE from a stored Row list (e.g. after
     * Cancel, to keep re-running the last-applied filter) doesn't have to
     * duplicate it */
    static QString buildWhere(const QVector<Row> &rows,
                              const std::function<QString(const QString &)> &quoteIdent,
                              const std::function<QString(const QString &)> &escapeValue);

private:
    struct RowWidgets
    {
        QComboBox *field;
        QComboBox *cond;
        QLineEdit *value;
    };
    QVector<RowWidgets> m_rowWidgets;
    QLineEdit *m_previewEdit = nullptr;
    QWidget *m_previewRow = nullptr;
    QCheckBox *m_previewToggle = nullptr;

    std::function<QString(const QString &)> m_quoteIdent;
    std::function<QString(const QString &)> m_escapeValue;
    std::function<QString(const QString &)> m_fullQueryFor;

    void updatePreview();
};
