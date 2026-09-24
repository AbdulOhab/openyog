#include "CustomFilterDialog.h"
#include "SqlEditor.h"

#include <QCheckBox>
#include <QPushButton>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace {
/* FT_EQUAL..FT_LIKE in upstream's SortAndFilter.h, same order */
const QStringList kConditions = {
    QStringLiteral("="), QStringLiteral("<>"),   QStringLiteral(">"),
    QStringLiteral("<"), QStringLiteral("LIKE"),
};
} // namespace

CustomFilterDialog::CustomFilterDialog(const QStringList &columns, const QVector<Row> &initial,
                                       std::function<QString(const QString &)> quoteIdent,
                                       std::function<QString(const QString &)> escapeValue,
                                       std::function<QString(const QString &)> fullQueryFor,
                                       QWidget *parent)
    : QDialog(parent), m_quoteIdent(std::move(quoteIdent)), m_escapeValue(std::move(escapeValue)),
      m_fullQueryFor(std::move(fullQueryFor))
{
    setWindowTitle(QStringLiteral("Custom Filter"));

    auto *grid = new QGridLayout;
    grid->addWidget(new QLabel(QStringLiteral("<b>Field</b>")), 0, 0);
    grid->addWidget(new QLabel(QStringLiteral("<b>Condition</b>")), 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("<b>Value</b>")), 0, 2);

    m_rowWidgets.reserve(kRows);
    for(int i = 0; i < kRows; ++i) {
        auto *field = new QComboBox(this);
        field->addItem(QString()); /* blank = skip this row */
        field->addItems(columns);
        auto *cond = new QComboBox(this);
        cond->addItems(kConditions);
        auto *value = new QLineEdit(this);

        if(i < initial.size() && !initial[i].field.isEmpty()) {
            field->setCurrentText(initial[i].field);
            const int ci = kConditions.indexOf(initial[i].condition);
            cond->setCurrentIndex(ci >= 0 ? ci : 0);
            value->setText(initial[i].value);
        }

        connect(field, &QComboBox::currentTextChanged, this, [this] { updatePreview(); });
        connect(cond, &QComboBox::currentTextChanged, this, [this] { updatePreview(); });
        connect(value, &QLineEdit::textChanged, this, [this] { updatePreview(); });

        const int row = i + 1;
        grid->addWidget(field, row, 0);
        grid->addWidget(cond, row, 1);
        grid->addWidget(value, row, 2);
        m_rowWidgets.append({field, cond, value});
    }
    grid->setColumnStretch(2, 1);

    /* SQLyog's "Show SQL Preview" / "Hide SQL Preview" is a text link that
     * flips (IDC_SHOWSQL in SQLyog.rc). A QLabel with rich-text link hit-
     * testing proved unreliable to click, so it is a flat button drawn as a
     * link: blue, underlined, hand cursor — a real widget, always clickable */
    m_previewToggle = new QPushButton(QStringLiteral("Show SQL Preview"), this);
    m_previewToggle->setFlat(true);
    m_previewToggle->setCursor(Qt::PointingHandCursor);
    m_previewToggle->setFocusPolicy(Qt::TabFocus);
    const bool darkBase = palette().color(QPalette::Base).lightness() < 128;
    m_previewToggle->setStyleSheet(
        QStringLiteral("QPushButton { border: 0; padding: 2px 0; text-align: left;"
                       " text-decoration: underline; background: transparent; color: %1; }"
                       "QPushButton:hover { color: %2; }")
            .arg(darkBase ? QStringLiteral("#7FB3E8") : QStringLiteral("#3B7DBB"),
                 darkBase ? QStringLiteral("#A9CDF3") : QStringLiteral("#1E5A96")));
    m_previewToggle->setAutoDefault(false); /* Enter must keep meaning OK */
    connect(m_previewToggle, &QPushButton::clicked, this, [this] {
        const bool on = !m_previewRow->isVisible();
        m_previewRow->setVisible(on);
        m_previewToggle->setText(on ? QStringLiteral("Hide SQL Preview")
                                    : QStringLiteral("Show SQL Preview"));
    });

    m_previewEdit = new SqlEditor(this);
    m_previewEdit->setReadOnly(true);
    m_previewEdit->setCaretLineVisible(false); /* looks like a stray edit cursor otherwise */
    m_previewEdit->setMarginWidth(0, 0);       /* no line-number gutter — one short statement */
    m_previewEdit->setWrapMode(
        QsciScintilla::WrapWord);      /* long AND-joined queries wrap, don't scroll */
    m_previewEdit->setFixedHeight(64); /* ~3 lines: enough for a few AND-joined rows */
    m_previewRow = new QWidget(this);
    auto *previewLayout = new QVBoxLayout(m_previewRow);
    previewLayout->setContentsMargins(0, 4, 0, 0);
    previewLayout->addWidget(new QLabel(QStringLiteral("Query"), m_previewRow));
    previewLayout->addWidget(m_previewEdit);
    m_previewRow->hide();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(grid);
    layout->addWidget(m_previewToggle);
    layout->addWidget(m_previewRow);
    layout->addStretch(1);
    layout->addWidget(buttons);
    setMinimumWidth(480);

    updatePreview();
}

void CustomFilterDialog::updatePreview()
{
    const QString where = whereClause();
    QString text = m_fullQueryFor
                       ? m_fullQueryFor(where)
                       : (where.isEmpty() ? QString() : QStringLiteral("WHERE %1").arg(where));
    if(text.isEmpty())
        text = QStringLiteral("-- no filter"); /* QsciScintilla has no placeholder text */
    m_previewEdit->setPlainText(text);
}

QVector<CustomFilterDialog::Row> CustomFilterDialog::rows() const
{
    QVector<Row> out;
    out.reserve(m_rowWidgets.size());
    for(const RowWidgets &w : m_rowWidgets)
        out.append({w.field->currentText(), w.cond->currentText(), w.value->text()});
    return out;
}

QString CustomFilterDialog::whereClause() const
{
    return buildWhere(rows(), m_quoteIdent, m_escapeValue);
}

QString CustomFilterDialog::buildWhere(const QVector<Row> &rows,
                                       const std::function<QString(const QString &)> &quoteIdent,
                                       const std::function<QString(const QString &)> &escapeValue)
{
    QStringList parts;
    for(const Row &r : rows) {
        if(r.field.isEmpty())
            continue;
        const QString col = quoteIdent(r.field);

        /* upstream: "=" / "<>" against a value of NULL or (NULL),
         * case-insensitively, becomes IS [NOT] NULL instead of a string
         * comparison — SetFilterString()'s special case */
        if(r.condition == QStringLiteral("=") || r.condition == QStringLiteral("<>")) {
            const QString v = r.value.trimmed();
            if(v.compare(QStringLiteral("NULL"), Qt::CaseInsensitive) == 0 ||
               v.compare(QStringLiteral("(NULL)"), Qt::CaseInsensitive) == 0) {
                parts << QStringLiteral("%1 IS %2NULL")
                             .arg(col, r.condition == QStringLiteral("<>") ? QStringLiteral("NOT ")
                                                                           : QString());
                continue;
            }
        }

        if(r.condition == QStringLiteral("LIKE")) {
            /* upstream: a leading/trailing '%' typed into Value means
             * "ends with" / "starts with" / "contains" (both) rather than
             * a literal leading/trailing '%' in the pattern — stripped
             * before escaping, then reattached around the escaped text
             * (ProcessFilter()'s FT_LIKEBEGIN/FT_LIKEEND/FT_LIKEBOTH) */
            QString v = r.value;
            const bool pre = v.startsWith(QLatin1Char('%'));
            if(pre)
                v = v.mid(1);
            const bool post = v.endsWith(QLatin1Char('%'));
            if(post)
                v.chop(1);
            parts << QStringLiteral("%1 LIKE '%2%3%4'")
                         .arg(col, pre ? QStringLiteral("%") : QString(), escapeValue(v),
                              post ? QStringLiteral("%") : QString());
            continue;
        }

        parts << QStringLiteral("%1 %2 '%3'").arg(col, r.condition, escapeValue(r.value));
    }
    return parts.join(QStringLiteral(" AND "));
}
