#include "InfoPanel.h"
#include "NavIcons.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

InfoPanel::InfoPanel(QWidget *parent) : QWidget(parent)
{
    m_view = new QTextBrowser(this);
    m_view->setOpenLinks(false);
    m_view->setHtml(
        QStringLiteral("<p style='color:#8a8a8a'>Select a table, view, procedure, function, "
                       "trigger, or event in the Object Browser to see its details here.</p>"));

    m_field = new QLineEdit(this);
    m_field->setClearButtonEnabled(true);
    m_field->setPlaceholderText(
        QStringLiteral("Search in Info…  (Enter = next, Shift+Enter = previous)"));
    m_field->installEventFilter(this);

    m_case = new QToolButton(this);
    m_case->setText(QStringLiteral("Aa"));
    m_case->setToolTip(QStringLiteral("Match case"));
    m_case->setCheckable(true);
    m_case->setAutoRaise(true);
    m_prev = new QToolButton(this);
    m_prev->setToolTip(QStringLiteral("Previous match (Shift+Enter)"));
    m_prev->setAutoRaise(true);
    m_next = new QToolButton(this);
    m_next->setToolTip(QStringLiteral("Next match (Enter)"));
    m_next->setAutoRaise(true);
    for(QToolButton *b : {m_case, m_prev, m_next}) {
        b->setObjectName(QStringLiteral("flatTool"));
        b->setFixedSize(30, 28);
    }
    m_case->setFont([this] {
        QFont f = font();
        f.setBold(true);
        return f;
    }());
    m_prev->setIconSize(QSize(16, 16));
    m_next->setIconSize(QSize(16, 16));
    applyIcons();
    m_count = new QLabel(this);
    m_count->setMinimumWidth(80);
    setStyleSheet(NavIcons::flatToolSheet());

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(6, 5, 6, 5);
    bar->setSpacing(4);
    bar->addWidget(m_field, 1);
    bar->addWidget(m_case);
    bar->addWidget(m_prev);
    bar->addWidget(m_next);
    bar->addWidget(m_count);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addLayout(bar);
    root->addWidget(m_view, 1);

    connect(m_field, &QLineEdit::textChanged, this, [this] { rescan(); });
    connect(m_case, &QToolButton::toggled, this, [this] { rescan(); });
    connect(m_prev, &QToolButton::clicked, this, [this] { step(-1); });
    connect(m_next, &QToolButton::clicked, this, [this] { step(+1); });
    rescan();
}

void InfoPanel::applyIcons()
{
    m_prev->setIcon(NavIcons::arrow(NavIcons::Up, palette()));
    m_next->setIcon(NavIcons::arrow(NavIcons::Down, palette()));
}

void InfoPanel::changeEvent(QEvent *event)
{
    if(event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        applyIcons(); /* live theme switch */
    QWidget::changeEvent(event);
}

void InfoPanel::setHtml(const QString &html)
{
    m_view->setHtml(html);
    rescan(); /* a kept search term applies to the new page too */
}

void InfoPanel::focusSearch()
{
    m_field->setFocus();
    m_field->selectAll();
}

bool InfoPanel::eventFilter(QObject *watched, QEvent *event)
{
    if(watched == m_field && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch(ke->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                step(ke->modifiers() & Qt::ShiftModifier ? -1 : +1);
                return true;
            case Qt::Key_F3:
                step(ke->modifiers() & Qt::ShiftModifier ? -1 : +1);
                return true;
            case Qt::Key_Escape:
                m_field->clear();
                return true;
            default:
                break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void InfoPanel::rescan()
{
    m_matches.clear();
    m_current = -1;
    const QString needle = m_field->text();
    if(!needle.isEmpty()) {
        QTextDocument *doc = m_view->document();
        QTextDocument::FindFlags flags;
        if(m_case->isChecked())
            flags |= QTextDocument::FindCaseSensitively;
        QTextCursor c(doc);
        while(true) {
            c = doc->find(needle, c, flags);
            if(c.isNull())
                break;
            m_matches << c;
        }
        if(!m_matches.isEmpty()) {
            /* start at the first match at or after the current scroll position */
            m_current = 0;
        }
    }
    paintHighlights();
    if(m_current >= 0)
        step(0);
}

void InfoPanel::step(int delta)
{
    if(m_matches.isEmpty())
        return;
    const int n = m_matches.size();
    m_current = ((m_current < 0 ? 0 : m_current) + delta + n) % n;
    paintHighlights();
    QTextCursor c = m_matches[m_current];
    /* the selection scrolls the view; setTextCursor() would steal focus from the field */
    QTextCursor view = c;
    view.clearSelection();
    m_view->setTextCursor(view);
    m_view->ensureCursorVisible();
}

void InfoPanel::paintHighlights()
{
    QList<QTextEdit::ExtraSelection> sels;
    for(int i = 0; i < m_matches.size(); ++i) {
        QTextEdit::ExtraSelection s;
        s.cursor = m_matches[i];
        s.format.setBackground(i == m_current ? QColor(0xFF, 0x9F, 0x43)
                                              : QColor(0xFF, 0xE5, 0x8A));
        s.format.setForeground(QColor(0x1E, 0x1E, 0x1E));
        sels << s;
    }
    m_view->setExtraSelections(sels);

    const bool none = m_matches.isEmpty() && !m_field->text().isEmpty();
    m_count->setText(m_field->text().isEmpty() ? QString()
                     : none ? QStringLiteral("No matches")
                            : QStringLiteral("%1 of %2").arg(m_current + 1).arg(m_matches.size()));
    m_field->setStyleSheet(
        none ? QStringLiteral("QLineEdit { background: #FDE7E9; color: #1E1E1E; }") : QString());
    m_prev->setEnabled(m_matches.size() > 1);
    m_next->setEnabled(m_matches.size() > 1);
}

void InfoPanel::setSearchForTest(const QString &text)
{
    m_field->setText(text);
}

QString InfoPanel::countTextForTest() const
{
    return m_count->text();
}
