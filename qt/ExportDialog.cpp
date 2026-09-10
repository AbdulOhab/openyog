#include "ExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

ExportDialog::ExportDialog(const QString &suggestedBaseName,
                           const QString &sqlTable, int rowCount,
                           bool haveSelection, QWidget *parent)
    : QDialog(parent), m_base(suggestedBaseName)
{
    setWindowTitle(QStringLiteral("Export %1 row(s)").arg(rowCount));

    m_format = new QComboBox(this);
    m_format->addItems(ResultExport::formatNames());

    m_path = new QLineEdit(this);
    auto *browse = new QPushButton(QStringLiteral("&Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &ExportDialog::pickFile);
    auto *pathRow = new QWidget(this);
    auto *pathLay = new QHBoxLayout(pathRow);
    pathLay->setContentsMargins(0, 0, 0, 0);
    pathLay->addWidget(m_path, 1);
    pathLay->addWidget(browse);

    m_delim = new QLineEdit(QStringLiteral(","), this);
    m_delim->setMaxLength(1);
    m_delim->setFixedWidth(48);
    m_quote = new QLineEdit(QStringLiteral("\""), this);
    m_quote->setMaxLength(1);
    m_quote->setFixedWidth(48);
    m_null = new QLineEdit(QStringLiteral("NULL"), this);
    m_sqlTable = new QLineEdit(sqlTable.isEmpty() ? QStringLiteral("exported")
                                                  : sqlTable, this);
    m_header = new QCheckBox(QStringLiteral("Write a header row"), this);
    m_header->setChecked(true);
    m_crlf = new QCheckBox(QStringLiteral("CRLF line endings"), this);
    m_crlf->setChecked(true);
    m_bom = new QCheckBox(QStringLiteral("UTF-8 BOM"), this);
    m_selOnly = new QCheckBox(
        QStringLiteral("Only the checked / selected rows"), this);
    m_selOnly->setChecked(haveSelection);
    m_selOnly->setEnabled(haveSelection);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Format"), m_format);
    form->addRow(QStringLiteral("File"), pathRow);
    m_delimLabel = new QLabel(QStringLiteral("Field separator"), this);
    form->addRow(m_delimLabel, m_delim);
    m_quoteLabel = new QLabel(QStringLiteral("Quote character"), this);
    form->addRow(m_quoteLabel, m_quote);
    m_sqlTableLabel = new QLabel(QStringLiteral("INSERT table name"), this);
    form->addRow(m_sqlTableLabel, m_sqlTable);
    form->addRow(QStringLiteral("NULL shown as"), m_null);
    form->addRow(QString(), m_header);
    form->addRow(QString(), m_crlf);
    form->addRow(QString(), m_bom);
    form->addRow(QString(), m_selOnly);

    auto *bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("&Export"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(bb);

    connect(m_format, &QComboBox::currentTextChanged,
            this, &ExportDialog::syncForFormat);
    syncForFormat();
}

void ExportDialog::syncForFormat()
{
    const auto f = format();
    const bool csvish = f == ResultExport::Format::Csv;
    const bool tsvish = f == ResultExport::Format::Tsv;
    const bool sql = f == ResultExport::Format::Sql;
    m_delimLabel->setVisible(csvish);
    m_delim->setVisible(csvish);
    m_quoteLabel->setVisible(csvish);
    m_quote->setVisible(csvish);
    m_sqlTableLabel->setVisible(sql);
    m_sqlTable->setVisible(sql);
    m_bom->setVisible(csvish || tsvish);
    m_crlf->setVisible(csvish || tsvish);
    m_header->setVisible(f != ResultExport::Format::Json
                         && f != ResultExport::Format::Sql
                         && f != ResultExport::Format::Xml);

    /* keep the path's suffix in step with the format */
    const QString suf = ResultExport::suffixFor(f);
    QString p = m_path->text().trimmed();
    if(p.isEmpty())
        p = QDir(QDir::homePath()).filePath(m_base + QLatin1Char('.') + suf);
    else {
        QFileInfo fi(p);
        p = fi.dir().filePath(fi.completeBaseName() + QLatin1Char('.') + suf);
    }
    m_path->setText(p);
}

void ExportDialog::pickFile()
{
    const QString suf = ResultExport::suffixFor(format());
    const QString p = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export to…"),
        m_path->text().isEmpty()
            ? QDir(QDir::homePath()).filePath(m_base + QLatin1Char('.') + suf)
            : m_path->text(),
        QStringLiteral("*.%1;;All files (*)").arg(suf));
    if(!p.isEmpty())
        m_path->setText(p);
}

QString ExportDialog::path() const { return m_path->text().trimmed(); }

ResultExport::Format ExportDialog::format() const
{
    return ResultExport::formatForName(m_format->currentText());
}

bool ExportDialog::selectionOnly() const
{
    return m_selOnly->isEnabled() && m_selOnly->isChecked();
}

ResultExport::Options ExportDialog::options() const
{
    ResultExport::Options o;
    o.delimiter = m_delim->text().isEmpty() ? QLatin1Char(',')
                                            : m_delim->text().at(0);
    o.quote = m_quote->text().isEmpty() ? QChar() : m_quote->text().at(0);
    o.nullText = m_null->text();
    o.header = m_header->isChecked();
    o.bom = m_bom->isChecked();
    o.lineEnd = m_crlf->isChecked() ? QStringLiteral("\r\n")
                                    : QStringLiteral("\n");
    o.sqlTable = m_sqlTable->text().trimmed().isEmpty()
                     ? QStringLiteral("exported")
                     : m_sqlTable->text().trimmed();
    return o;
}
