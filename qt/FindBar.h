/* OpenYog — inline find bar for the query editor (Ctrl+F).
 * A slim strip under the editor: field + prev/next + case toggle + match
 * count + close. Operates on whatever CodeEditor the accessor returns, so it
 * follows the active Query tab. Esc (or the ✕) hides it and returns focus. */
#pragma once

#include <QWidget>
#include <functional>

class CodeEditor;
class QLabel;
class QLineEdit;
class QToolButton;

class FindBar : public QWidget
{
    Q_OBJECT
public:
    explicit FindBar(std::function<CodeEditor *()> editorAccessor,
                     QWidget *parent = nullptr);

    void activate();          /* show, prefill from selection, focus the field */
    void findNext(bool backward = false);

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    void updateCount();

    std::function<CodeEditor *()> m_editor;
    QLineEdit   *m_field = nullptr;
    QToolButton *m_case  = nullptr;
    QLabel      *m_count = nullptr;
};
