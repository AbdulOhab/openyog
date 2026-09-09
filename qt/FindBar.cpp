#include "FindBar.h"
#include "CodeEditor.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

FindBar::FindBar(std::function<CodeEditor *()> editorAccessor, QWidget *parent)
    : QWidget(parent), m_editor(std::move(editorAccessor))
{
    m_field = new QLineEdit(this);
    m_field->setPlaceholderText(QStringLiteral("Find"));
    m_field->setClearButtonEnabled(true);

    m_case = new QToolButton(this);
    m_case->setText(QStringLiteral("Aa"));
    m_case->setCheckable(true);
    m_case->setToolTip(QStringLiteral("Case sensitive"));

    auto *prev = new QToolButton(this);
    prev->setArrowType(Qt::UpArrow);
    prev->setToolTip(QStringLiteral("Previous (Shift+Enter)"));
    auto *next = new QToolButton(this);
    next->setArrowType(Qt::DownArrow);
    next->setToolTip(QStringLiteral("Next (Enter)"));

    m_count = new QLabel(this);
    m_count->setMinimumWidth(64);

    auto *close = new QToolButton(this);
    close->setText(QStringLiteral("✕"));
    close->setAutoRaise(true);

    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(4, 2, 4, 2);
    l->setSpacing(3);
    l->addWidget(new QLabel(QStringLiteral("Find:"), this));
    l->addWidget(m_field, 1);
    l->addWidget(m_case);
    l->addWidget(prev);
    l->addWidget(next);
    l->addWidget(m_count);
    l->addWidget(close);

    connect(m_field, &QLineEdit::textChanged, this, [this] {
        updateCount();
        findNext(false);          /* incremental */
    });
    connect(m_field, &QLineEdit::returnPressed, this, [this] { findNext(false); });
    connect(prev, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(next, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(m_case, &QToolButton::toggled, this, [this] {
        updateCount();
        findNext(false);
    });
    connect(close, &QToolButton::clicked, this, [this] {
        hide();
        if(auto *e = m_editor()) e->setFocus();
    });

    hide();
}

void FindBar::activate()
{
    if(auto *e = m_editor()) {
        const QString sel = e->textCursor().selectedText();
        if(!sel.isEmpty() && !sel.contains(QChar::ParagraphSeparator))
            m_field->setText(sel);
    }
    show();
    m_field->setFocus();
    m_field->selectAll();
    updateCount();
}

void FindBar::findNext(bool backward)
{
    auto *e = m_editor();
    const QString needle = m_field->text();
    if(!e || needle.isEmpty())
        return;
    const bool found = e->findText(needle, m_case->isChecked(), backward);
    m_field->setStyleSheet(found ? QString()
                                 : QStringLiteral("background:#F8D7DA;"));
}

void FindBar::updateCount()
{
    auto *e = m_editor();
    const QString needle = m_field->text();
    if(!e || needle.isEmpty()) {
        m_count->clear();
        return;
    }
    const int n = e->toPlainText().count(
        needle, m_case->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive);
    m_count->setText(n ? QStringLiteral("%1 match%2").arg(n).arg(n == 1 ? "" : "es")
                       : QStringLiteral("no matches"));
}

void FindBar::keyPressEvent(QKeyEvent *e)
{
    if(e->key() == Qt::Key_Escape) {
        hide();
        if(auto *ed = m_editor()) ed->setFocus();
        return;
    }
    if((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
       && (e->modifiers() & Qt::ShiftModifier)) {
        findNext(true);
        return;
    }
    QWidget::keyPressEvent(e);
}
