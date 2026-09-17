#include "CustomFilterDialog.h"

#include <QCheckBox>
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
                                       QWidget *parent)
    : QDialog(parent), m_quoteIdent(std::move(quoteIdent)), m_escapeValue(std::move(escapeValue))
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

    /* a QLabel styled as a clickable "Show/Hide SQL Preview" link looked
     * right but was unreliable to actually click (Qt's rich-text link hit-
     * testing on a plain QLabel — confirmed both by the owner on a real
     * desktop and by a synthesized click here landing on nothing); a plain
     * checkbox is completely reliable and just as compact */
    m_previewToggle = new QCheckBox(QStringLiteral("Show SQL Preview"), this);
    connect(m_previewToggle, &QCheckBox::toggled, this,
            [this](bool on) { m_previewRow->setVisible(on); });

    m_previewEdit = new QLineEdit(this);
    m_previewEdit->setReadOnly(true);
    m_previewEdit->setPlaceholderText(QStringLiteral("(no filter)"));
    m_previewRow = new QWidget(this);
    auto *previewLayout = new QVBoxLayout(m_previewRow);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->addWidget(new QLabel(QStringLiteral("WHERE"), m_previewRow));
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
    m_previewEdit->setText(whereClause());
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
