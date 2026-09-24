#include "FormView.h"
#include "Icons.h"

#include <QCheckBox>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QEvent>
#include <QIcon>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QIntValidator>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
const QString kNull = QStringLiteral("NULL");

QToolButton *navButton(const QString &text, const QString &tip, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}

/* first / previous / next / last arrows, painted in the palette's own colours
 * so they stay crisp and readable on every theme (an icon file would be one
 * fixed colour); a separate greyed pixmap for the disabled state */
QIcon navIcon(int kind, const QPalette &pal)
{
    QIcon icon;
    const struct
    {
        QIcon::Mode mode;
        QColor color;
    } modes[] = {{QIcon::Normal, pal.color(QPalette::Active, QPalette::ButtonText)},
                 {QIcon::Disabled, pal.color(QPalette::Disabled, QPalette::ButtonText)}};
    for(const auto &m : modes) {
        QPixmap pm(32, 32);
        pm.setDevicePixelRatio(2);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(m.color);
        const auto tri = [&](qreal tipX, qreal baseX) {
            const QPointF pts[3] = {{baseX, 3.5}, {tipX, 8.0}, {baseX, 12.5}};
            p.drawPolygon(pts, 3);
        };
        switch(kind) {
            case 0: /* first: bar + left triangle */
                p.drawRoundedRect(QRectF(2.6, 3.5, 1.9, 9.0), 0.6, 0.6);
                tri(5.6, 12.2);
                break;
            case 1:
                tri(4.6, 11.4);
                break;
            case 2:
                tri(11.4, 4.6);
                break;
            default: /* last */
                p.drawRoundedRect(QRectF(11.5, 3.5, 1.9, 9.0), 0.6, 0.6);
                tri(10.4, 3.8);
                break;
        }
        icon.addPixmap(pm, m.mode);
    }
    return icon;
}

QIcon fileIcon(const QString &name)
{
    return Icons::get(name);
}

/* long-text column types get a multi-line box */
bool wantsMultiLine(const QString &type)
{
    const QString t = type.toLower();
    return t.contains(QLatin1String("text")) || t.contains(QLatin1String("json")) ||
           t.contains(QLatin1String("xml"));
}

/* the same tints the grid uses for staged cells; dark text so they stay
 * readable on the dark themes too */
QString tint(int state, bool dirty)
{
    const char *bg = state == FormView::Deleted    ? "#FDE7E9"
                     : state == FormView::Inserted ? "#E6F4EA"
                     : dirty                       ? "#FFF3C4"
                                                   : nullptr;
    return bg ? QStringLiteral("background: %1; color: #1E1E1E;").arg(QLatin1String(bg))
              : QString();
}
} // namespace

FormView::FormView(QWidget *parent) : QWidget(parent)
{
    m_first = navButton(QString(), QStringLiteral("First row"), this);
    m_prev = navButton(QString(), QStringLiteral("Previous row"), this);
    m_next = navButton(QString(), QStringLiteral("Next row"), this);
    m_last = navButton(QString(), QStringLiteral("Last row"), this);
    for(QToolButton *b : {m_first, m_prev, m_next, m_last}) {
        b->setIconSize(QSize(16, 16));
        b->setFixedSize(30, 28);
        b->setObjectName(QStringLiteral("formNav"));
    }
    m_goto = new QLineEdit(this);
    m_goto->setValidator(new QIntValidator(1, 99999999, m_goto));
    m_goto->setAlignment(Qt::AlignCenter);
    m_goto->setFixedWidth(64);
    m_goto->setToolTip(QStringLiteral("Go to row (Enter)"));
    m_count = new QLabel(this);
    m_new = navButton(QStringLiteral("New row"), QStringLiteral("Stage a new, empty row"), this);
    m_dup = navButton(QStringLiteral("Duplicate"),
                      QStringLiteral("Stage a new row copied from this one"), this);
    m_del = navButton(QStringLiteral("Delete"), QStringLiteral("Mark this row for deletion"), this);
    m_new->setIcon(fileIcon(QStringLiteral("result_insert.ico")));
    m_dup->setIcon(fileIcon(QStringLiteral("duplicaterow.ico")));
    m_del->setIcon(fileIcon(QStringLiteral("result_delete.ico")));
    for(QToolButton *b : {m_new, m_dup, m_del}) {
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIconSize(QSize(16, 16));
        b->setMinimumHeight(28);
        b->setObjectName(QStringLiteral("formAction"));
    }
    /* thin divider between the groups */
    const auto divider = [this] {
        auto *f = new QFrame(this);
        f->setFixedSize(1, 20);
        f->setStyleSheet(QStringLiteral("background: rgba(127, 127, 127, 110);"));
        return f;
    };
    auto *rowLabel = new QLabel(QStringLiteral("Row"), this);
    auto *nav = new QHBoxLayout;
    nav->setContentsMargins(8, 6, 8, 6);
    nav->setSpacing(4);
    nav->addWidget(m_first);
    nav->addWidget(m_prev);
    nav->addSpacing(4);
    nav->addWidget(rowLabel);
    nav->addWidget(m_goto);
    nav->addWidget(m_count);
    nav->addSpacing(4);
    nav->addWidget(m_next);
    nav->addWidget(m_last);
    nav->addSpacing(10);
    nav->addWidget(divider());
    nav->addSpacing(10);
    nav->addWidget(m_new);
    nav->addWidget(m_dup);
    nav->addWidget(m_del);
    nav->addStretch(1);

    /* toolbar-style buttons: flat until hovered, then a soft frame — colours are
     * translucent greys so the same sheet suits light and dark themes */
    setStyleSheet(QStringLiteral(
        "QToolButton#formNav, QToolButton#formAction {"
        " border: 1px solid transparent; border-radius: 4px; padding: 2px 8px; }"
        "QToolButton#formNav { padding: 2px; }"
        "QToolButton#formNav:hover:enabled, QToolButton#formAction:hover:enabled {"
        " background: rgba(127, 127, 127, 45); border-color: rgba(127, 127, 127, 110); }"
        "QToolButton#formNav:pressed:enabled, QToolButton#formAction:pressed:enabled {"
        " background: rgba(127, 127, 127, 90); }"
        "QToolButton#formAction[danger=\"true\"]:hover:enabled { background: rgba(220, 70, 70, 60);"
        " border-color: rgba(220, 70, 70, 140); }"));
    m_del->setObjectName(QStringLiteral("formAction"));
    m_del->setProperty("danger", true);
    applyNavIcons();

    m_banner = new QLabel(this);
    m_banner->setContentsMargins(8, 3, 8, 3);
    m_banner->setStyleSheet(QStringLiteral("color: #1E1E1E;"));
    m_banner->hide();

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_body = new QWidget;
    m_scroll->setWidget(m_body);

    m_empty = new QLabel(QStringLiteral("No rows — use “New row” to add one."), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setEnabled(false);
    m_empty->hide();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addLayout(nav);
    root->addWidget(m_banner);
    root->addWidget(m_scroll, 1);
    root->addWidget(m_empty, 1);

    connect(m_first, &QToolButton::clicked, this, [this] { setCurrentRow(0); });
    connect(m_prev, &QToolButton::clicked, this, [this] { setCurrentRow(m_row - 1); });
    connect(m_next, &QToolButton::clicked, this, [this] { setCurrentRow(m_row + 1); });
    connect(m_last, &QToolButton::clicked, this,
            [this] { setCurrentRow(m_model ? m_model->rowCount() - 1 : 0); });
    connect(m_goto, &QLineEdit::editingFinished, this, [this] {
        if(m_loading)
            return;
        bool ok = false;
        const int v = m_goto->text().toInt(&ok);
        if(ok && v - 1 != m_row)
            setCurrentRow(v - 1);
        else
            updateNav(); /* restore the shown number */
    });
    connect(m_new, &QToolButton::clicked, this, [this] {
        commit();
        emit newRowRequested();
    });
    connect(m_dup, &QToolButton::clicked, this, [this] {
        commit();
        emit duplicateRequested(m_row);
    });
    connect(m_del, &QToolButton::clicked, this, [this] {
        commit();
        emit deleteRequested(m_row);
    });
}

void FormView::applyNavIcons()
{
    m_first->setIcon(navIcon(0, palette()));
    m_prev->setIcon(navIcon(1, palette()));
    m_next->setIcon(navIcon(2, palette()));
    m_last->setIcon(navIcon(3, palette()));
}

void FormView::changeEvent(QEvent *event)
{
    if(event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        applyNavIcons(); /* live theme switch */
    QWidget::changeEvent(event);
}

void FormView::setModel(QAbstractItemModel *model, const Hooks &hooks)
{
    m_model = model;
    m_hooks = hooks;
    const auto changed = [this] {
        if(m_loading)
            return;
        const int n = m_model->rowCount();
        m_row = qBound(0, m_row, qMax(0, n - 1));
        refresh();
    };
    connect(m_model, &QAbstractItemModel::modelReset, this, changed);
    connect(m_model, &QAbstractItemModel::rowsInserted, this, changed);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, changed);
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &tl, const QModelIndex &br) {
                if(!m_loading && tl.row() <= m_row && m_row <= br.row())
                    refresh();
            });
}

void FormView::setColumns(const QList<Column> &columns)
{
    bool same = columns.size() == m_columns.size();
    for(int i = 0; same && i < columns.size(); ++i)
        same = columns[i].name == m_columns[i].name && columns[i].type == m_columns[i].type &&
               columns[i].primary == m_columns[i].primary &&
               columns[i].nullable == m_columns[i].nullable &&
               columns[i].autoInc == m_columns[i].autoInc && columns[i].blob == m_columns[i].blob;
    m_columns = columns;
    if(!same)
        rebuild();
    if(m_model)
        m_row = qBound(0, m_row, qMax(0, m_model->rowCount() - 1));
    refresh();
}

void FormView::rebuild()
{
    /* the old body (and every field in it) goes away wholesale */
    m_fields.clear();
    m_body = new QWidget;
    auto *grid = new QGridLayout(m_body);
    grid->setContentsMargins(12, 8, 12, 12);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);

    const QFontMetrics fm(font());
    for(int c = 0; c < m_columns.size(); ++c) {
        const Column &col = m_columns[c];
        Field f;
        f.label = new QLabel(m_body);
        f.label->setTextFormat(Qt::RichText);
        f.label->setText(QStringLiteral("<b>%1</b>%2 <span style='color:gray'>%3</span>")
                             .arg(col.name.toHtmlEscaped(),
                                  col.primary ? QStringLiteral(" 🔑") : QString(),
                                  col.type.toHtmlEscaped()));
        f.label->setAlignment(Qt::AlignRight | Qt::AlignTop);
        f.label->setToolTip(col.nullable ? QString() : QStringLiteral("NOT NULL"));

        if(wantsMultiLine(col.type) && !col.blob) {
            f.multi = new QPlainTextEdit(m_body);
            f.multi->setFixedHeight(fm.lineSpacing() * 5 + 10);
            f.multi->installEventFilter(this);
        } else {
            f.line = new QLineEdit(m_body);
            connect(f.line, &QLineEdit::editingFinished, this, [this, c] { commitField(c); });
        }
        f.nullBox = new QCheckBox(QStringLiteral("NULL"), m_body);
        f.nullBox->setEnabled(col.nullable && !col.blob);
        f.nullBox->setVisible(!col.blob);
        connect(f.nullBox, &QCheckBox::toggled, this, [this, c](bool on) {
            if(m_loading || !m_model || m_row >= m_model->rowCount())
                return;
            m_model->setData(m_model->index(m_row, c), on ? kNull : QString(), Qt::EditRole);
            if(!on) {
                const Field &fld = m_fields[c];
                QWidget *w =
                    fld.line ? static_cast<QWidget *>(fld.line) : static_cast<QWidget *>(fld.multi);
                w->setFocus();
            }
        });

        QWidget *editor = f.line ? static_cast<QWidget *>(f.line) : static_cast<QWidget *>(f.multi);
        grid->addWidget(f.label, c, 0, Qt::AlignTop);
        grid->addWidget(editor, c, 1);
        grid->addWidget(f.nullBox, c, 2, Qt::AlignTop);
        m_fields << f;
    }
    grid->setRowStretch(m_columns.size(), 1);

    /* swap the scroll area's widget; setWidget() deletes the previous one */
    m_scroll->setWidget(m_body);
}

QString FormView::fieldText(const Field &f) const
{
    return f.line ? f.line->text() : f.multi->toPlainText();
}

QString FormView::modelText(int row, int col) const
{
    return m_model->data(m_model->index(row, col), Qt::EditRole).toString();
}

void FormView::styleField(const Field &f, int col, int state)
{
    const bool dirty = m_hooks.dirty && m_hooks.dirty(m_row, col);
    const QString css = tint(state, dirty);
    QWidget *w = f.line ? static_cast<QWidget *>(f.line) : static_cast<QWidget *>(f.multi);
    w->setStyleSheet(css.isEmpty() ? QString()
                                   : QStringLiteral("QLineEdit, QPlainTextEdit { %1 }").arg(css));
}

void FormView::refresh()
{
    if(!m_model)
        return;
    const int n = m_model->rowCount();
    const bool have = n > 0 && !m_columns.isEmpty();
    m_scroll->setVisible(have);
    m_empty->setVisible(!have);
    m_empty->setText(m_columns.isEmpty() ? QStringLiteral("Open a table to see its rows here.")
                                         : QStringLiteral("No rows — use “New row” to add one."));
    m_banner->hide();
    if(have) {
        m_loading = true;
        const int state = m_hooks.rowState ? m_hooks.rowState(m_row) : Normal;
        for(int c = 0; c < m_fields.size() && c < m_model->columnCount(); ++c) {
            const Column &col = m_columns[c];
            const Field &f = m_fields[c];
            const QString value = modelText(m_row, c);
            const bool isNull = value == kNull;
            const bool untouchedBlob =
                col.blob && state == Normal && !(m_hooks.dirty && m_hooks.dirty(m_row, c));
            QString shown = isNull ? QString() : value;
            if(untouchedBlob && !isNull)
                shown = QStringLiteral("<BLOB>");
            const bool locked = state == Deleted || col.blob;
            if(f.line) {
                f.line->setText(shown);
                f.line->setReadOnly(locked);
                f.line->setEnabled(!isNull);
                f.line->setPlaceholderText(isNull ? QStringLiteral("NULL")
                                                  : (state == Inserted && col.autoInc
                                                         ? QStringLiteral("(auto)")
                                                         : QString()));
            } else {
                f.multi->setPlainText(shown);
                f.multi->setReadOnly(locked);
                f.multi->setEnabled(!isNull);
                f.multi->setPlaceholderText(isNull ? QStringLiteral("NULL") : QString());
            }
            f.nullBox->setChecked(isNull);
            f.nullBox->setEnabled(col.nullable && !col.blob && state != Deleted);
            styleField(f, c, state);
        }
        m_loading = false;
        if(state == Inserted) {
            m_banner->setText(
                QStringLiteral("New row — fill in the fields; empty ones take the column default. "
                               "Apply to insert."));
            m_banner->setStyleSheet(QStringLiteral("background: #E6F4EA; color: #1E1E1E;"));
            m_banner->show();
        } else if(state == Deleted) {
            m_banner->setText(
                QStringLiteral("Marked for deletion — Apply to delete, Revert to undo."));
            m_banner->setStyleSheet(QStringLiteral("background: #FDE7E9; color: #1E1E1E;"));
            m_banner->show();
        }
        m_del->setText(state == Deleted ? QStringLiteral("Undo delete") : QStringLiteral("Delete"));
    }
    updateNav();
}

void FormView::updateNav()
{
    const int n = m_model ? m_model->rowCount() : 0;
    m_loading = true;
    m_goto->setText(QString::number(m_row + 1));
    m_loading = false;
    m_goto->setEnabled(n > 0);
    m_count->setText(QStringLiteral("of %1").arg(n));
    m_first->setEnabled(n > 0 && m_row > 0);
    m_prev->setEnabled(n > 0 && m_row > 0);
    m_next->setEnabled(n > 0 && m_row < n - 1);
    m_last->setEnabled(n > 0 && m_row < n - 1);
    m_new->setEnabled(!m_columns.isEmpty());
    m_dup->setEnabled(n > 0);
    m_del->setEnabled(n > 0);
}

void FormView::setCurrentRow(int row)
{
    if(!m_model)
        return;
    commit(); /* typed-but-uncommitted text belongs to the row we are leaving */
    const int n = m_model->rowCount();
    row = qBound(0, row, qMax(0, n - 1));
    const bool moved = row != m_row;
    m_row = row;
    refresh();
    if(moved)
        emit currentRowChanged(m_row);
}

void FormView::commitField(int col)
{
    if(m_loading || !m_model || col < 0 || col >= m_fields.size() || m_row >= m_model->rowCount())
        return;
    const Column &c = m_columns[col];
    const Field &f = m_fields[col];
    if(c.blob || f.nullBox->isChecked())
        return;
    const int state = m_hooks.rowState ? m_hooks.rowState(m_row) : Normal;
    if(state == Deleted)
        return;
    const QString text = fieldText(f);
    if(text != modelText(m_row, col))
        m_model->setData(m_model->index(m_row, col), text, Qt::EditRole);
}

void FormView::commit()
{
    for(int c = 0; c < m_fields.size(); ++c)
        commitField(c);
}

bool FormView::eventFilter(QObject *watched, QEvent *event)
{
    if(event->type() == QEvent::FocusOut)
        for(int c = 0; c < m_fields.size(); ++c)
            if(m_fields[c].multi == watched) {
                commitField(c);
                break;
            }
    return QWidget::eventFilter(watched, event);
}

void FormView::setFieldForTest(int col, const QString &text)
{
    if(col < 0 || col >= m_fields.size())
        return;
    const Field &f = m_fields[col];
    if(f.line)
        f.line->setText(text);
    else
        f.multi->setPlainText(text);
    commitField(col);
}

void FormView::setNullForTest(int col, bool on)
{
    if(col >= 0 && col < m_fields.size())
        m_fields[col].nullBox->setChecked(on);
}

QString FormView::fieldTextForTest(int col) const
{
    return col >= 0 && col < m_fields.size() ? fieldText(m_fields[col]) : QString();
}
