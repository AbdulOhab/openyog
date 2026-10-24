/* OpenYog — the Import External Data wizard; see ExternalDataDialog.h. */
#include "ExternalDataDialog.h"
#include "db/DbDriverRegistry.h"
#include "db/IDbConnection.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QStackedWidget>
#include <QTableWidget>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

using namespace ExternalData;

namespace {

constexpr int kPreviewRows = 20;
/* how many rows a new table's types are guessed from */
constexpr int kGuessRows = 100000;

enum MappingColumn { MSource, MTarget, MType };

QString skipText()
{
    return QStringLiteral("(skip)");
}

QWidget *row(QWidget *parent, std::initializer_list<QWidget *> widgets, int stretchIndex = 0)
{
    auto *w = new QWidget(parent);
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    int i = 0;
    for(QWidget *x : widgets)
        l->addWidget(x, i++ == stretchIndex ? 1 : 0);
    return w;
}

} // namespace

ExternalDataDialog::ExternalDataDialog(Connection target, QList<Connection> sources,
                                       QWidget *parent)
    : QWizard(parent), m_target(std::move(target)), m_sources(std::move(sources))
{
    setWindowTitle(QStringLiteral("Import External Data — %1").arg(m_target.label));
    setPage(SourcePage, makeSourcePage());
    setPage(TargetPage, makeTargetPage());
    setPage(ColumnsPage, makeColumnsPage());
    setPage(RunPage, makeRunPage());
    setOption(QWizard::NoBackButtonOnStartPage);
    setButtonText(QWizard::FinishButton, QStringLiteral("&Import"));
    resize(780, 580);
    sourceChanged();
}

ExternalDataDialog::~ExternalDataDialog()
{
    if(m_cancel)
        *m_cancel = true; /* a running import stops and rolls back */
}

/* ---- 1. source ------------------------------------------------------------ */

QWizardPage *ExternalDataDialog::makeSourcePage()
{
    auto *page = new QWizardPage(this);
    page->setTitle(QStringLiteral("Where the data comes from"));
    page->setSubTitle(QStringLiteral(
        "A CSV file, an Excel workbook (.xlsx), or a table or query of another open connection."));

    auto *csv = new QRadioButton(QStringLiteral("&CSV file"), page);
    auto *excel = new QRadioButton(QStringLiteral("&Excel workbook"), page);
    auto *db = new QRadioButton(QStringLiteral("Another &connection"), page);
    db->setEnabled(!m_sources.isEmpty());
    csv->setChecked(true);
    m_kindGroup = new QButtonGroup(page);
    m_kindGroup->addButton(csv, Csv);
    m_kindGroup->addButton(excel, Excel);
    m_kindGroup->addButton(db, Database);

    const auto browse = [this, page](QLineEdit *edit, const QString &filter) {
        auto *b = new QPushButton(QStringLiteral("Browse…"), page);
        connect(b, &QPushButton::clicked, this, [this, edit, filter] {
            const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Choose a File"),
                                                              edit->text(), filter);
            if(!file.isEmpty())
                edit->setText(file);
        });
        return b;
    };

    /* CSV */
    auto *csvPage = new QWidget(page);
    m_csvFile = new QLineEdit(csvPage);
    m_separator = new QComboBox(csvPage);
    m_separator->addItem(QStringLiteral("Found from the file"), QString());
    m_separator->addItem(QStringLiteral("Comma ( , )"), QStringLiteral(","));
    m_separator->addItem(QStringLiteral("Semicolon ( ; )"), QStringLiteral(";"));
    m_separator->addItem(QStringLiteral("Tab"), QStringLiteral("\t"));
    m_separator->addItem(QStringLiteral("Pipe ( | )"), QStringLiteral("|"));
    m_encoding = new QComboBox(csvPage);
    m_encoding->addItem(QStringLiteral("UTF-8"), int(CsvOptions::Utf8));
    m_encoding->addItem(QStringLiteral("UTF-16"), int(CsvOptions::Utf16));
    m_encoding->addItem(QStringLiteral("Latin-1 (ISO 8859-1)"), int(CsvOptions::Latin1));
    m_csvHeader = new QCheckBox(QStringLiteral("The first row has the column &names"), csvPage);
    m_csvHeader->setChecked(true);
    auto *csvForm = new QFormLayout(csvPage);
    csvForm->setContentsMargins(0, 0, 0, 0);
    csvForm->addRow(
        QStringLiteral("&File"),
        row(csvPage, {m_csvFile, browse(m_csvFile, QStringLiteral("CSV and text files (*.csv *.tsv "
                                                                  "*.txt);;All files (*)"))}));
    csvForm->addRow(QStringLiteral("&Separator"), m_separator);
    csvForm->addRow(QStringLiteral("E&ncoding"), m_encoding);
    csvForm->addRow(QString(), m_csvHeader);

    /* Excel */
    auto *excelPage = new QWidget(page);
    m_excelFile = new QLineEdit(excelPage);
    m_sheet = new QComboBox(excelPage);
    m_excelHeader = new QCheckBox(QStringLiteral("The first row has the column &names"), excelPage);
    m_excelHeader->setChecked(true);
    auto *excelForm = new QFormLayout(excelPage);
    excelForm->setContentsMargins(0, 0, 0, 0);
    excelForm->addRow(
        QStringLiteral("&File"),
        row(excelPage, {m_excelFile, browse(m_excelFile, QStringLiteral("Excel workbooks (*.xlsx);;"
                                                                        "All files (*)"))}));
    excelForm->addRow(QStringLiteral("S&heet"), m_sheet);
    excelForm->addRow(QString(), m_excelHeader);
    auto *xlsNote = new QLabel(
        QStringLiteral("An older .xls workbook: save it as .xlsx or CSV first."), excelPage);
    xlsNote->setEnabled(false);
    excelForm->addRow(QString(), xlsNote);

    /* another connection */
    auto *dbPage = new QWidget(page);
    m_sourceConn = new QComboBox(dbPage);
    for(const Connection &c : std::as_const(m_sources))
        m_sourceConn->addItem(c.label);
    m_sourceDb = new QComboBox(dbPage);
    m_sourceTable = new QComboBox(dbPage);
    auto *useTable = new QRadioButton(QStringLiteral("&Table"), dbPage);
    useTable->setChecked(true);
    m_useQuery = new QRadioButton(QStringLiteral("&Query"), dbPage);
    m_query = new QPlainTextEdit(dbPage);
    m_query->setPlaceholderText(QStringLiteral("SELECT … FROM …"));
    m_query->setMaximumHeight(70);
    m_query->setEnabled(false);
    connect(m_useQuery, &QRadioButton::toggled, m_query, &QWidget::setEnabled);
    connect(m_useQuery, &QRadioButton::toggled, m_sourceTable,
            [this](bool q) { m_sourceTable->setEnabled(!q); });
    auto *dbForm = new QFormLayout(dbPage);
    dbForm->setContentsMargins(0, 0, 0, 0);
    dbForm->addRow(QStringLiteral("C&onnection"), m_sourceConn);
    dbForm->addRow(QStringLiteral("&Database"), m_sourceDb);
    dbForm->addRow(useTable, m_sourceTable);
    dbForm->addRow(m_useQuery, m_query);

    m_kindStack = new QStackedWidget(page);
    m_kindStack->addWidget(csvPage);
    m_kindStack->addWidget(excelPage);
    m_kindStack->addWidget(dbPage);
    m_emptyIsNull =
        new QCheckBox(QStringLiteral("An empty value is &NULL (a quoted \"\" stays empty "
                                     "text)"),
                      page);
    m_emptyIsNull->setChecked(true);

    m_preview = new QTableWidget(page);
    m_preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_preview->verticalHeader()->setDefaultSectionSize(m_preview->fontMetrics().height() + 6);
    m_previewNote = new QLabel(page);
    m_previewNote->setWordWrap(true);

    auto *kinds = new QHBoxLayout;
    kinds->addWidget(csv);
    kinds->addWidget(excel);
    kinds->addWidget(db);
    kinds->addStretch(1);
    auto *lay = new QVBoxLayout(page);
    lay->addLayout(kinds);
    lay->addWidget(m_kindStack);
    lay->addWidget(m_emptyIsNull);
    lay->addWidget(m_previewNote);
    lay->addWidget(m_preview, 1);

    /* any change: the preview again, a moment after the last one */
    auto *debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(250);
    connect(debounce, &QTimer::timeout, this, &ExternalDataDialog::refreshPreview);
    const auto changed = [this, debounce] {
        sourceChanged();
        debounce->start();
    };
    connect(m_kindGroup, &QButtonGroup::idToggled, this, [changed](int, bool on) {
        if(on)
            changed();
    });
    for(QLineEdit *e : {m_csvFile, m_excelFile})
        connect(e, &QLineEdit::textChanged, this, changed);
    connect(m_excelFile, &QLineEdit::textChanged, this, &ExternalDataDialog::loadSheets);
    for(QComboBox *c : {m_separator, m_encoding, m_sheet, m_sourceTable})
        connect(c, &QComboBox::currentIndexChanged, this, changed);
    for(QCheckBox *c : {m_csvHeader, m_excelHeader, m_emptyIsNull})
        connect(c, &QCheckBox::toggled, this, changed);
    connect(m_useQuery, &QRadioButton::toggled, this, changed);
    connect(m_query, &QPlainTextEdit::textChanged, this, changed);
    connect(m_sourceConn, &QComboBox::currentIndexChanged, this, [this] { fillSourceDatabases(); });
    connect(m_sourceDb, &QComboBox::currentIndexChanged, this, [this] { fillSourceTables(); });
    fillSourceDatabases();
    return page;
}

ExternalDataDialog::Kind ExternalDataDialog::kind() const
{
    return Kind(std::max(0, m_kindGroup->checkedId()));
}

void ExternalDataDialog::sourceChanged()
{
    m_kindStack->setCurrentIndex(int(kind()));
    m_emptyIsNull->setVisible(kind() != Database);
}

void ExternalDataDialog::fillSourceDatabases()
{
    const int i = m_sourceConn->currentIndex();
    m_sourceDb->blockSignals(true);
    m_sourceDb->clear();
    if(i >= 0 && i < m_sources.size()) {
        m_sourceDb->addItems(m_sources[i].databases());
        m_sourceDb->setCurrentText(m_sources[i].currentDatabase());
    }
    m_sourceDb->blockSignals(false);
    fillSourceTables();
}

void ExternalDataDialog::fillSourceTables()
{
    const int i = m_sourceConn->currentIndex();
    m_sourceTable->blockSignals(true);
    m_sourceTable->clear();
    if(i >= 0 && i < m_sources.size())
        if(IDbConnection *c = m_sources[i].connection(m_sourceDb->currentText()))
            m_sourceTable->addItems(c->listTables(m_sourceDb->currentText()));
    m_sourceTable->blockSignals(false);
    if(kind() == Database)
        refreshPreview();
}

void ExternalDataDialog::loadSheets()
{
    QString error;
    const QStringList sheets = QFileInfo::exists(m_excelFile->text())
                                   ? xlsxSheets(m_excelFile->text(), &error)
                                   : QStringList();
    const QString was = m_sheet->currentText();
    m_sheet->blockSignals(true);
    m_sheet->clear();
    m_sheet->addItems(sheets);
    if(sheets.contains(was))
        m_sheet->setCurrentText(was);
    m_sheet->blockSignals(false);
}

Reader ExternalDataDialog::reader(bool forWorker) const
{
    switch(kind()) {
        case Csv: {
            CsvOptions o;
            o.separator = m_separator->currentData().toString().isEmpty()
                              ? QChar()
                              : m_separator->currentData().toString().at(0);
            o.header = m_csvHeader->isChecked();
            o.emptyIsNull = m_emptyIsNull->isChecked();
            o.encoding = CsvOptions::Encoding(m_encoding->currentData().toInt());
            return csvReader(m_csvFile->text().trimmed(), o);
        }
        case Excel:
            return xlsxReader(m_excelFile->text().trimmed(), m_sheet->currentText(),
                              m_excelHeader->isChecked(), m_emptyIsNull->isChecked());
        case Database:
            break;
    }
    const int i = m_sourceConn->currentIndex();
    if(i < 0 || i >= m_sources.size())
        return [](const OnColumns &, const EachRow &, QString *error) {
            *error = QStringLiteral("No connection to read from.");
            return false;
        };
    const QString db = m_sourceDb->currentText();
    const QString table = m_useQuery->isChecked() ? QString() : m_sourceTable->currentText();
    const QString query = m_useQuery->isChecked() ? m_query->toPlainText() : QString();
    if(!forWorker) {
        /* the tab's own connection: lists and previews on this thread */
        IDbConnection *c = m_sources[i].connection(db);
        return
            [c, db, table, query](const OnColumns &onColumns, const EachRow &each, QString *error) {
                if(!c) {
                    *error = QStringLiteral("The source connection is closed.");
                    return false;
                }
                return readConnection(c, db, table, query, onColumns, each, error);
            };
    }
    /* a connection of its own, opened and closed on the worker */
    const ConnectionParams params = m_sources[i].params(db);
    return [params, db, table, query](const OnColumns &onColumns, const EachRow &each,
                                      QString *error) {
        QString err;
        IDbDriver *driver = dbDriverFor(params.driverType);
        std::unique_ptr<IDbConnection> c(driver ? driver->connect(params, &err) : nullptr);
        if(!c) {
            *error = QStringLiteral("Can't connect to the source: %1").arg(err);
            return false;
        }
        return readConnection(c.get(), db, table, query, onColumns, each, error);
    };
}

QString ExternalDataDialog::sourceName() const
{
    switch(kind()) {
        case Csv:
            return QFileInfo(m_csvFile->text()).completeBaseName();
        case Excel:
            return QFileInfo(m_excelFile->text()).completeBaseName();
        case Database:
            break;
    }
    return m_useQuery->isChecked() ? QStringLiteral("query_result") : m_sourceTable->currentText();
}

void ExternalDataDialog::refreshPreview()
{
    m_preview->clear();
    m_preview->setRowCount(0);
    m_preview->setColumnCount(0);
    const bool fileMissing = (kind() == Csv && !QFileInfo::exists(m_csvFile->text().trimmed())) ||
                             (kind() == Excel && !QFileInfo::exists(m_excelFile->text().trimmed()));
    if(fileMissing ||
       (kind() == Database && !m_useQuery->isChecked() && m_sourceTable->currentText().isEmpty()) ||
       (kind() == Database && m_useQuery->isChecked() &&
        m_query->toPlainText().trimmed().isEmpty())) {
        m_previewNote->setText(kind() == Database
                                   ? QStringLiteral("Choose a table, or write a query.")
                                   : QStringLiteral("Choose a file."));
        return;
    }
    QStringList columns;
    QList<Row> rows;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = reader(false)([&](const QStringList &c) { columns = c; },
                                  [&](const Row &r) {
                                      rows << r;
                                      return rows.size() < kPreviewRows;
                                  },
                                  &error);
    QApplication::restoreOverrideCursor();
    if(!ok) {
        m_previewNote->setText(error);
        m_previewNote->setStyleSheet(QStringLiteral("color: #c0392b"));
        return;
    }
    m_previewNote->setStyleSheet(QString());
    m_previewNote->setText(columns.isEmpty()
                               ? QStringLiteral("Nothing to import: the source is empty.")
                               : QStringLiteral("%1 columns; the first rows:").arg(columns.size()));
    m_preview->setColumnCount(int(columns.size()));
    m_preview->setHorizontalHeaderLabels(columns);
    m_preview->setRowCount(int(rows.size()));
    const QColor muted = palette().color(QPalette::Disabled, QPalette::Text);
    for(int r = 0; r < rows.size(); ++r)
        for(int c = 0; c < columns.size() && c < rows[r].values.size(); ++c) {
            auto *item = new QTableWidgetItem;
            if(rows[r].nulls[c]) {
                item->setText(QStringLiteral("NULL"));
                item->setForeground(muted);
            } else {
                item->setText(QString::fromUtf8(rows[r].values[c]).left(200));
            }
            m_preview->setItem(r, c, item);
        }
}

/* ---- 2. target ------------------------------------------------------------ */

QWizardPage *ExternalDataDialog::makeTargetPage()
{
    auto *page = new QWizardPage(this);
    page->setTitle(QStringLiteral("Where it goes"));
    page->setSubTitle(
        QStringLiteral("A table of %1: a new one, or one already there.").arg(m_target.label));
    m_targetDb = new QComboBox(page);
    m_targetDb->addItems(m_target.databases());
    m_targetDb->setCurrentText(m_target.currentDatabase());
    m_newTable = new QRadioButton(QStringLiteral("A &new table"), page);
    m_newTable->setChecked(true);
    m_existing = new QRadioButton(QStringLiteral("An e&xisting table"), page);
    m_tableName = new QLineEdit(page);
    m_existingTable = new QComboBox(page);
    m_existingTable->setEnabled(false);
    m_emptyFirst =
        new QCheckBox(QStringLiteral("Delete its rows &first (undone if the import fails)"), page);
    m_emptyFirst->setEnabled(false);
    connect(m_newTable, &QRadioButton::toggled, this, [this](bool isNew) {
        m_tableName->setEnabled(isNew);
        m_existingTable->setEnabled(!isNew);
        m_emptyFirst->setEnabled(!isNew);
    });
    connect(m_targetDb, &QComboBox::currentIndexChanged, this,
            &ExternalDataDialog::fillTargetTables);
    auto *form = new QFormLayout(page);
    form->addRow(QStringLiteral("&Database"), m_targetDb);
    form->addRow(m_newTable, m_tableName);
    form->addRow(m_existing, m_existingTable);
    form->addRow(QString(), m_emptyFirst);
    fillTargetTables();
    return page;
}

void ExternalDataDialog::fillTargetTables()
{
    const QString was = m_existingTable->currentText();
    m_existingTable->clear();
    if(IDbConnection *c = m_target.connection(m_targetDb->currentText()))
        m_existingTable->addItems(
            c->listTables(m_targetDb->currentText(), QStringLiteral("BASE TABLE")));
    m_existingTable->setCurrentText(was);
}

/* ---- 3. columns ----------------------------------------------------------- */

QWizardPage *ExternalDataDialog::makeColumnsPage()
{
    auto *page = new QWizardPage(this);
    page->setTitle(QStringLiteral("Columns"));
    m_mapping = new QTableWidget(0, 3, page);
    m_mapping->setHorizontalHeaderLabels(
        {QStringLiteral("Source column"), QStringLiteral("Target column"), QStringLiteral("Type")});
    m_mapping->horizontalHeader()->setStretchLastSection(true);
    m_mapping->verticalHeader()->hide();
    m_mapping->setColumnWidth(MSource, 220);
    m_mapping->setColumnWidth(MTarget, 220);
    m_mappingNote = new QLabel(page);
    m_mappingNote->setWordWrap(true);
    auto *lay = new QVBoxLayout(page);
    lay->addWidget(m_mappingNote);
    lay->addWidget(m_mapping, 1);
    return page;
}

void ExternalDataDialog::buildMapping()
{
    const bool isNew = m_newTable->isChecked();
    const QString db = m_targetDb->currentText();
    /* the source's columns, and for a new table their types */
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_columns.clear();
    QList<TypeGuess> guesses;
    QString error;
    const bool fromTable = kind() == Database && !m_useQuery->isChecked();
    reader(false)(
        [&](const QStringList &c) {
            m_columns = c;
            guesses.resize(c.size());
        },
        [&, n = 0](const Row &r) mutable {
            if(!isNew || fromTable)
                return false; /* the names are all it needs */
            for(int i = 0; i < r.values.size() && i < guesses.size(); ++i)
                guesses[i].add(r.values[i], r.nulls[i]);
            return ++n < kGuessRows;
        },
        &error);
    m_types.clear();
    m_binary.clear();
    for(const TypeGuess &g : std::as_const(guesses))
        m_types << g.type();
    m_binary = QList<bool>(m_columns.size(), false);
    if(fromTable) {
        /* the source table's own types */
        const int i = m_sourceConn->currentIndex();
        if(IDbConnection *c = m_sources[i].connection(m_sourceDb->currentText())) {
            const DbResultSet cols =
                c->listColumns(m_sourceDb->currentText(), m_sourceTable->currentText());
            for(int k = 0; k < m_columns.size() && k < cols.rows.size(); ++k) {
                m_types[k] = fromSqlType(cols.rows[k].value(1));
                m_binary[k] = m_types[k].kind == ColumnType::Binary;
            }
        }
    }
    QStringList targetColumns, targetTypes;
    if(!isNew)
        if(IDbConnection *c = m_target.connection(db))
            for(const QStringList &r : c->listColumns(db, m_existingTable->currentText()).rows) {
                targetColumns << r.value(0);
                targetTypes << r.value(1);
            }
    QApplication::restoreOverrideCursor();

    m_mappingNote->setText(
        isNew ? QStringLiteral("Untick a column to leave it out. The types are a guess from the "
                               "data%1: change any of them.")
                    .arg(fromTable ? QStringLiteral(" (the source table's own)") : QString())
              : QStringLiteral("Each source column goes into the target column picked for it, or "
                               "nowhere."));
    m_mapping->clearContents();
    m_mapping->setRowCount(int(m_columns.size()));
    const SqlDriverType driver = m_target.driver;
    for(int i = 0; i < m_columns.size(); ++i) {
        auto *src = new QTableWidgetItem(m_columns[i]);
        src->setFlags((src->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        src->setCheckState(Qt::Checked);
        m_mapping->setItem(i, MSource, src);
        if(isNew) {
            m_mapping->setCellWidget(i, MTarget, nullptr);
            m_mapping->setItem(i, MTarget,
                               new QTableWidgetItem(identifier(
                                   m_columns[i], QStringLiteral("column%1").arg(i + 1))));
            auto *type = new QComboBox(m_mapping);
            type->setEditable(true);
            const ColumnType guessed = m_types.value(i);
            QStringList choices{sqlType(guessed, driver)};
            ColumnType decimal{ColumnType::Decimal};
            decimal.precision = 18;
            decimal.scale = 2;
            for(const ColumnType &t :
                {ColumnType{ColumnType::Integer}, ColumnType{ColumnType::BigInt}, decimal,
                 ColumnType{ColumnType::Double}, ColumnType{ColumnType::Boolean},
                 ColumnType{ColumnType::Date}, ColumnType{ColumnType::DateTime},
                 ColumnType{ColumnType::Text}, ColumnType{ColumnType::LongText},
                 ColumnType{ColumnType::Binary}})
                if(!choices.contains(sqlType(t, driver)))
                    choices << sqlType(t, driver);
            type->addItems(choices);
            type->setCurrentIndex(0);
            type->setToolTip(QStringLiteral("Guessed: %1").arg(describe(guessed)));
            m_mapping->setCellWidget(i, MType, type);
        } else {
            m_mapping->setItem(i, MTarget, nullptr);
            auto *target = new QComboBox(m_mapping);
            target->addItem(skipText());
            target->addItems(targetColumns);
            /* the same name, whatever its case; otherwise left out */
            int match = 0;
            for(int k = 0; k < targetColumns.size() && !match; ++k)
                if(targetColumns[k].compare(m_columns[i], Qt::CaseInsensitive) == 0)
                    match = k + 1;
            target->setCurrentIndex(match);
            auto *typeItem = new QTableWidgetItem(targetTypes.value(match - 1));
            typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
            m_mapping->setItem(i, MType, typeItem);
            connect(target, &QComboBox::currentIndexChanged, this, [this, i, targetTypes](int k) {
                if(QTableWidgetItem *t = m_mapping->item(i, MType))
                    t->setText(targetTypes.value(k - 1));
            });
            m_mapping->setCellWidget(i, MType, nullptr);
            m_mapping->setCellWidget(i, MTarget, target);
        }
    }
}

Plan ExternalDataDialog::plan(QString *error) const
{
    Plan p;
    p.database = m_targetDb->currentText();
    p.createTable = m_newTable->isChecked();
    p.table = p.createTable ? m_tableName->text().trimmed() : m_existingTable->currentText();
    p.emptyFirst = !p.createTable && m_emptyFirst->isChecked();
    QSet<QString> used;
    for(int i = 0; i < m_mapping->rowCount(); ++i) {
        if(m_mapping->item(i, MSource)->checkState() != Qt::Checked)
            continue;
        Mapping m;
        m.source = i;
        m.binary = m_binary.value(i);
        if(p.createTable) {
            m.target = m_mapping->item(i, MTarget) ? m_mapping->item(i, MTarget)->text().trimmed()
                                                   : QString();
            m.type = m_types.value(i);
            if(auto *type = qobject_cast<QComboBox *>(m_mapping->cellWidget(i, MType)))
                if(type->currentText().trimmed() != sqlType(m.type, m_target.driver))
                    m.declared = type->currentText().trimmed();
        } else {
            auto *target = qobject_cast<QComboBox *>(m_mapping->cellWidget(i, MTarget));
            if(!target || target->currentIndex() <= 0)
                continue;
            m.target = target->currentText();
        }
        if(m.target.isEmpty()) {
            if(error)
                *error = QStringLiteral("Column %1 has no target name.").arg(m_columns.value(i));
            return {};
        }
        if(used.contains(m.target.toLower())) {
            if(error)
                *error = QStringLiteral("Two columns go into %1.").arg(m.target);
            return {};
        }
        used.insert(m.target.toLower());
        p.columns << m;
    }
    if(p.columns.isEmpty() && error)
        *error = QStringLiteral("No column goes anywhere: tick or pick at least one.");
    return p;
}

/* ---- 4. run ----------------------------------------------------------------- */

QWizardPage *ExternalDataDialog::makeRunPage()
{
    auto *page = new QWizardPage(this);
    page->setTitle(QStringLiteral("Import"));
    m_summary = new QLabel(page);
    m_summary->setWordWrap(true);
    m_progress = new QProgressBar(page);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_status = new QLabel(page);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *lay = new QVBoxLayout(page);
    lay->addWidget(m_summary);
    lay->addSpacing(8);
    lay->addWidget(m_progress);
    lay->addWidget(m_status);
    lay->addStretch(1);
    return page;
}

void ExternalDataDialog::initializePage(int id)
{
    if(id == TargetPage) {
        /* a name from the source, until one is typed */
        if(m_tableName->text().isEmpty() || !m_tableName->isModified())
            m_tableName->setText(identifier(sourceName(), QStringLiteral("imported")));
        fillTargetTables();
    } else if(id == ColumnsPage) {
        buildMapping();
    } else if(id == RunPage) {
        QString error;
        const Plan p = plan(&error);
        m_done = false;
        m_progress->setRange(0, 1);
        m_progress->setValue(0);
        m_status->clear();
        setButtonText(QWizard::FinishButton, QStringLiteral("&Import"));
        m_summary->setText(
            QStringLiteral("%1 column%2 of %3 into %4.%5, %6. Every row goes in, or none does.")
                .arg(p.columns.size())
                .arg(p.columns.size() == 1 ? "" : "s")
                .arg(kind() == Database
                         ? QStringLiteral("%1 on %2").arg(sourceName(), m_sourceConn->currentText())
                         : QFileInfo(kind() == Csv ? m_csvFile->text() : m_excelFile->text())
                               .fileName())
                .arg(p.database, p.table,
                     p.createTable  ? QStringLiteral("a new table")
                     : p.emptyFirst ? QStringLiteral("its rows deleted first")
                                    : QStringLiteral("added to its rows")));
    }
    QWizard::initializePage(id);
}

bool ExternalDataDialog::validateCurrentPage()
{
    QString error;
    switch(currentId()) {
        case SourcePage: {
            QStringList columns;
            const bool ok = reader(false)([&](const QStringList &c) { columns = c; },
                                          [](const Row &) { return false; }, &error);
            if(ok && columns.isEmpty())
                error = QStringLiteral("Nothing to import: the source has no columns.");
            break;
        }
        case TargetPage:
            if(m_newTable->isChecked()) {
                const QString name = m_tableName->text().trimmed();
                QStringList tables;
                if(IDbConnection *c = m_target.connection(m_targetDb->currentText()))
                    tables = c->listTables(m_targetDb->currentText());
                if(name.isEmpty())
                    error = QStringLiteral("Name the new table.");
                else if(tables.contains(name, Qt::CaseInsensitive))
                    error = QStringLiteral("%1 already has a table %2: pick \"An existing table\" "
                                           "to add to it, or another name.")
                                .arg(m_targetDb->currentText(), name);
            } else if(m_existingTable->currentText().isEmpty()) {
                error = QStringLiteral("Choose the table.");
            }
            break;
        case ColumnsPage:
            plan(&error);
            break;
        case RunPage:
            if(m_done)
                return true; /* Close */
            if(!m_running)
                startImport();
            return false;
    }
    if(!error.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return false;
    }
    return true;
}

void ExternalDataDialog::startImport()
{
    QString error;
    const Plan p = plan(&error);
    if(!error.isEmpty()) {
        m_status->setText(error);
        return;
    }
    m_running = true;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    m_progress->setRange(0, 0); /* busy: the row count isn't known */
    m_status->setStyleSheet(QString());
    m_status->setText(QStringLiteral("Importing…"));
    button(QWizard::FinishButton)->setEnabled(false);
    button(QWizard::BackButton)->setEnabled(false);
    setButtonText(QWizard::CancelButton, QStringLiteral("&Stop"));

    const Reader source = reader(true);
    const ConnectionParams params = m_target.params(p.database);
    const auto cancel = m_cancel;
    QPointer<ExternalDataDialog> self(this);
    QThreadPool::globalInstance()->start([self, source, params, p, cancel] {
        QString err;
        qint64 rows = 0;
        IDbDriver *driver = dbDriverFor(params.driverType);
        std::unique_ptr<IDbConnection> c(driver ? driver->connect(params, &err) : nullptr);
        bool ok = false;
        if(!c) {
            err = QStringLiteral("Can't connect: %1").arg(err);
        } else {
            ok = run(
                c.get(), p, source, &rows, &err,
                [self](qint64 n) {
                    QMetaObject::invokeMethod(
                        qApp,
                        [self, n] {
                            if(self && self->m_running)
                                self->m_status->setText(
                                    QStringLiteral("%1 rows so far…").arg(QLocale().toString(n)));
                        },
                        Qt::QueuedConnection);
                },
                [cancel] { return cancel->load(); });
        }
        QMetaObject::invokeMethod(
            qApp,
            [self, ok, rows, err, p] {
                if(!self)
                    return;
                self->m_running = false;
                self->m_progress->setRange(0, 1);
                self->m_progress->setValue(ok ? 1 : 0);
                self->setButtonText(QWizard::CancelButton, QStringLiteral("Cancel"));
                self->button(QWizard::BackButton)->setEnabled(!ok);
                self->button(QWizard::FinishButton)->setEnabled(true);
                if(ok) {
                    self->m_done = true;
                    self->m_result = QStringLiteral("Imported %1 rows into %2.%3.")
                                         .arg(QLocale().toString(rows), p.database, p.table);
                    self->m_status->setText(self->m_result);
                    self->setButtonText(QWizard::FinishButton, QStringLiteral("&Close"));
                    self->button(QWizard::CancelButton)->hide(); /* nothing left to cancel */
                    emit self->imported(p.database, p.table);
                } else {
                    self->m_result = err;
                    self->m_status->setText(err);
                    self->m_status->setStyleSheet(QStringLiteral("color: #c0392b"));
                }
            },
            Qt::QueuedConnection);
    });
}

void ExternalDataDialog::reject()
{
    if(m_running) {
        *m_cancel = true; /* it stops after the batch going in, and rolls back */
        m_status->setText(QStringLiteral("Stopping…"));
        return;
    }
    QWizard::reject();
}

/* ---- the selftests' way through ------------------------------------------ */

void ExternalDataDialog::setCsvSourceForTest(const QString &file)
{
    m_kindGroup->button(Csv)->setChecked(true);
    m_csvFile->setText(file);
}

void ExternalDataDialog::setExcelSourceForTest(const QString &file, const QString &sheet)
{
    m_kindGroup->button(Excel)->setChecked(true);
    m_excelFile->setText(file);
    m_sheet->setCurrentText(sheet);
}

void ExternalDataDialog::setDatabaseSourceForTest(int connection, const QString &db,
                                                  const QString &table)
{
    m_kindGroup->button(Database)->setChecked(true);
    m_sourceConn->setCurrentIndex(connection);
    m_sourceDb->setCurrentText(db);
    m_sourceTable->setCurrentText(table);
}

void ExternalDataDialog::setNewTableForTest(const QString &db, const QString &table)
{
    m_targetDb->setCurrentText(db);
    m_newTable->setChecked(true);
    m_tableName->setText(table);
    m_tableName->setModified(true);
}

void ExternalDataDialog::setExistingTableForTest(const QString &db, const QString &table,
                                                 bool emptyFirst)
{
    m_targetDb->setCurrentText(db);
    fillTargetTables();
    m_existing->setChecked(true);
    m_existingTable->setCurrentText(table);
    m_emptyFirst->setChecked(emptyFirst);
}

QStringList ExternalDataDialog::mappingForTest()
{
    refreshPreview();
    while(currentId() != ColumnsPage) {
        const int before = currentId();
        next();
        if(currentId() == before)
            return {QStringLiteral("stuck on page %1").arg(before)};
    }
    QStringList out;
    for(int i = 0; i < m_mapping->rowCount(); ++i) {
        QString target, type;
        if(auto *c = qobject_cast<QComboBox *>(m_mapping->cellWidget(i, MTarget)))
            target = c->currentText();
        else if(m_mapping->item(i, MTarget))
            target = m_mapping->item(i, MTarget)->text();
        if(auto *c = qobject_cast<QComboBox *>(m_mapping->cellWidget(i, MType)))
            type = c->currentText();
        else if(m_mapping->item(i, MType))
            type = m_mapping->item(i, MType)->text();
        out << QStringLiteral("%1 -> %2 %3").arg(m_mapping->item(i, MSource)->text(), target, type);
    }
    return out;
}

QString ExternalDataDialog::importForTest()
{
    if(currentId() != ColumnsPage)
        mappingForTest();
    next(); /* to the run page */
    if(currentId() != RunPage)
        return QStringLiteral("not on the run page");
    validateCurrentPage(); /* Import */
    QElapsedTimer t;
    t.start();
    while(m_running && t.elapsed() < 60000) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(5);
    }
    QApplication::processEvents();
    return m_result;
}
