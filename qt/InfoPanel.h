/* OpenYog — the "Info" result tab: the object details page (columns, indexes,
 * foreign keys, DDL) with a search strip on top. Typing highlights every match
 * in the page (current one in a stronger colour), Enter / F3 jump to the next,
 * Shift+Enter / Shift+F3 to the previous, Esc clears. The page is re-highlighted
 * whenever a new object is shown, so a term can be kept while clicking through
 * tables. */
#pragma once

#include <QList>
#include <QTextCursor>
#include <QWidget>

class QLabel;
class QLineEdit;
class QTextBrowser;
class QToolButton;

class InfoPanel : public QWidget
{
    Q_OBJECT
public:
    explicit InfoPanel(QWidget *parent = nullptr);

    void setHtml(const QString &html);
    void focusSearch(); /* Ctrl+F while this tab is in front */

    /* selftest */
    void setSearchForTest(const QString &text);
    int matchCountForTest() const
    {
        return m_matches.size();
    }
    QString countTextForTest() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rescan();        /* find every match, repaint the highlights */
    void step(int delta); /* move the current match, scroll to it */
    void paintHighlights();

    QTextBrowser *m_view = nullptr;
    QLineEdit *m_field = nullptr;
    QToolButton *m_case = nullptr;
    QToolButton *m_prev = nullptr;
    QToolButton *m_next = nullptr;
    QLabel *m_count = nullptr;
    QList<QTextCursor> m_matches;
    int m_current = -1;
};
