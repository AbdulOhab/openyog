/* OpenYog — the Schema Synchronization Tool; see SchemaSyncDialog.h. */
#include "SchemaSyncDialog.h"
#include "Icons.h"
#include "SqlEditor.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

using SchemaSync::Action;
using SchemaSync::Change;

namespace {

QString driverName(SqlDriverType d)
{
    switch(d) {
        case SqlDriverType::Mysql:
            return QStringLiteral("MySQL");
        case SqlDriverType::Postgres:
            return QStringLiteral("PostgreSQL");
        case SqlDriverType::Sqlite:
            break;
    }
    return QStringLiteral("SQLite");
}

bool doable(const Change &c)
{
    return !c.sql.isEmpty() || !c.prepare.isEmpty();
}

/* a definition on one line, for the list (the whole of it in the tooltip) */
QString oneLine(const QString &definition)
{
    return definition.simplified();
}

} // namespace

SchemaSyncDialog::SchemaSyncDialog(QList<Endpoint> endpoints, QWidget *parent)
    : QDialog(parent), m_endpoints(std::move(endpoints))
{
    setWindowTitle(QStringLiteral("Schema Synchronization"));
    setWindowIcon(Icons::get(QStringLiteral("schema.ico")));
    resize(1000, 700);

    const auto side = [this](const QString &title, QComboBox *&conn, QComboBox *&db) {
        auto *box = new QGroupBox(title, this);
        auto *form = new QFormLayout(box);
        conn = new QComboBox(box);
        for(const Endpoint &e : std::as_const(m_endpoints))
            conn->addItem(QStringLiteral("%1 (%2)").arg(e.label, driverName(e.driver)));
        db = new QComboBox(box);
        db->setMinimumWidth(200);
        form->addRow(QStringLiteral("Connection"), conn);
        form->addRow(QStringLiteral("Database"), db);
        QComboBox *dbBox = db;
        connect(conn, &QComboBox::currentIndexChanged, this, [this, dbBox](int i) {
            if(i >= 0)
                fillDatabases(dbBox, i, m_endpoints[i].currentDatabase());
        });
        return box;
    };
    auto *sourceBox = side(QStringLiteral("Source: the schema to copy"), m_sourceConn, m_sourceDb);
    auto *targetBox =
        side(QStringLiteral("Target: the database to change"), m_targetConn, m_targetDb);
    auto *arrow = new QLabel(QStringLiteral("→"), this);
    arrow->setStyleSheet(QStringLiteral("font-size: 20pt;"));
    auto *compare = new QPushButton(QStringLiteral("&Compare"), this);
    compare->setDefault(true);
    connect(compare, &QPushButton::clicked, this, &SchemaSyncDialog::compareNow);
    auto *sides = new QHBoxLayout;
    sides->addWidget(sourceBox, 1);
    sides->addWidget(arrow);
    sides->addWidget(targetBox, 1);
    sides->addWidget(compare, 0, Qt::AlignBottom);

    m_status = new QLabel(QStringLiteral("Pick the two databases and compare them. Nothing is "
                                         "changed here: the script is yours to run."),
                          this);
    m_status->setWordWrap(true);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("Difference"), QStringLiteral("In the source"),
                             QStringLiteral("In the target")});
    m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_tree->setColumnWidth(0, 280);
    m_tree->setColumnWidth(1, 330);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this] {
        if(!m_filling)
            updateScript();
    });

    m_script = new SqlEditor(this);
    m_script->setReadOnly(true);

    auto *split = new QSplitter(Qt::Vertical, this);
    split->addWidget(m_tree);
    split->addWidget(m_script);
    split->setSizes({380, 260});

    auto *buttons = new QDialogButtonBox(this);
    m_open = buttons->addButton(QStringLiteral("Open in Target's &Query Editor"),
                                QDialogButtonBox::ActionRole);
    m_save = buttons->addButton(QStringLiteral("&Save Script…"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_open, &QPushButton::clicked, this, [this] {
        if(m_targetEndpoint >= 0 && m_endpoints[m_targetEndpoint].openInEditor)
            m_endpoints[m_targetEndpoint].openInEditor(scriptText());
    });
    connect(m_save, &QPushButton::clicked, this, &SchemaSyncDialog::saveScript);
    m_open->setEnabled(false);
    m_save->setEnabled(false);

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(sides);
    lay->addWidget(m_status);
    lay->addWidget(split, 1);
    lay->addWidget(buttons);

    /* the current connection as the source; the next one, if any, as the
     * target */
    fillDatabases(m_sourceDb, 0, m_endpoints.value(0).currentDatabase());
    const int target = m_endpoints.size() > 1 ? 1 : 0;
    m_targetConn->setCurrentIndex(target);
    fillDatabases(m_targetDb, target, m_endpoints.value(target).currentDatabase());
}

void SchemaSyncDialog::fillDatabases(QComboBox *databases, int endpoint, const QString &select)
{
    databases->clear();
    if(endpoint < 0 || endpoint >= m_endpoints.size())
        return;
    databases->addItems(m_endpoints[endpoint].databases());
    const int at = databases->findText(select);
    if(at >= 0)
        databases->setCurrentIndex(at);
}

void SchemaSyncDialog::compareForTest(int source, const QString &sourceDb, int target,
                                      const QString &targetDb)
{
    m_sourceConn->setCurrentIndex(source);
    fillDatabases(m_sourceDb, source, sourceDb);
    m_targetConn->setCurrentIndex(target);
    fillDatabases(m_targetDb, target, targetDb);
    compareNow();
}

void SchemaSyncDialog::compareNow()
{
    const int s = m_sourceConn->currentIndex(), t = m_targetConn->currentIndex();
    if(s < 0 || t < 0)
        return;
    m_filling = true;
    m_tree->clear();
    m_changeItems.clear();
    m_changes.clear();
    m_filling = false;
    m_targetEndpoint = -1;
    if(m_endpoints[s].driver != m_endpoints[t].driver) {
        m_status->setText(
            QStringLiteral("The source is %1 and the target %2: Schema Sync compares "
                           "two databases of the same kind.")
                .arg(driverName(m_endpoints[s].driver), driverName(m_endpoints[t].driver)));
        updateScript();
        return;
    }
    const QString sourceDb = m_sourceDb->currentText(), targetDb = m_targetDb->currentText();
    if(s == t && sourceDb == targetDb) {
        m_status->setText(QStringLiteral("The source and the target are the same database."));
        updateScript();
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_source = m_endpoints[s].load(sourceDb);
    m_target = m_endpoints[t].load(targetDb);
    QApplication::restoreOverrideCursor();
    m_targetEndpoint = t;
    m_changes = SchemaSync::compare(m_source, m_target);

    m_filling = true;
    QHash<QString, QTreeWidgetItem *> tables;
    for(const Change &c : std::as_const(m_changes)) {
        /* a table's changes under it; a view, routine or trigger on its own */
        const QString group = c.kind + QLatin1Char('\n') + c.table;
        QTreeWidgetItem *table = tables.value(group);
        if(!table) {
            static const QHash<QString, QString> icons = {
                {QString(), QStringLiteral("table.ico")},
                {QStringLiteral("VIEW"), QStringLiteral("alterview.ico")},
                {QStringLiteral("PROCEDURE"), QStringLiteral("process.ico")},
                {QStringLiteral("FUNCTION"), QStringLiteral("alterfunction.ico")},
                {QStringLiteral("TRIGGER"), QStringLiteral("altertrigger.ico")}};
            table = new QTreeWidgetItem(m_tree, {c.table});
            table->setIcon(0, Icons::get(icons.value(c.kind, QStringLiteral("table.ico"))));
            table->setFlags(table->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            table->setCheckState(0, Qt::Unchecked);
            table->setExpanded(true);
            tables.insert(group, table);
        }
        QString what = SchemaSync::describe(c);
        if(c.action == Action::DropTable || c.action == Action::DropColumn)
            what += QStringLiteral(" (loses its data)");
        auto *item = new QTreeWidgetItem(table, {what, oneLine(c.source), oneLine(c.target)});
        item->setToolTip(1, c.source);
        item->setToolTip(2, c.target);
        if(doable(c)) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, c.pickedByDefault ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(0, (c.prepare + c.sql).join(QStringLiteral(";\n")));
        } else {
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            item->setText(0, what + QStringLiteral(": can't be done here"));
            item->setToolTip(0, c.note);
            item->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
        }
        m_changeItems << item;
    }
    m_filling = false;
    updateScript();
}

QList<Change> SchemaSyncDialog::pickedChanges() const
{
    QList<Change> out;
    for(int i = 0; i < m_changes.size(); ++i)
        if(doable(m_changes[i]) && m_changeItems[i]->checkState(0) == Qt::Checked)
            out << m_changes[i];
    return out;
}

void SchemaSyncDialog::updateScript()
{
    const QList<Change> picked = pickedChanges();
    QList<Change> forScript = picked;
    for(const Change &c : std::as_const(m_changes))
        if(!doable(c))
            forScript << c; /* listed under "Not done here" */
    m_script->setReadOnly(false);
    if(m_targetEndpoint < 0)
        m_script->setPlainText(QString());
    else if(m_changes.isEmpty())
        m_script->setPlainText(QStringLiteral("-- nothing to do: %1 already matches %2\n")
                                   .arg(m_target.database, m_source.database));
    else
        m_script->setPlainText(SchemaSync::script(forScript, m_source, m_target));
    m_script->setReadOnly(true);
    if(m_targetEndpoint >= 0)
        m_status->setText(m_changes.isEmpty()
                              ? QStringLiteral("%1 and %2 have the same tables, columns, keys, "
                                               "foreign keys, views, routines and triggers.")
                                    .arg(m_source.database, m_target.database)
                              : QStringLiteral("%1 differences, %2 picked. Dropping a table, a "
                                               "column, a view, routine or trigger, and table "
                                               "options, start unticked.")
                                    .arg(m_changes.size())
                                    .arg(picked.size()));
    m_open->setEnabled(!picked.isEmpty());
    m_save->setEnabled(!picked.isEmpty());
}

QString SchemaSyncDialog::scriptText() const
{
    return m_script->toPlainText();
}

void SchemaSyncDialog::saveScript()
{
    const QString file = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save Script"),
        QStringLiteral("sync-%1-to-%2.sql").arg(m_source.database, m_target.database),
        QStringLiteral("SQL (*.sql)"));
    if(file.isEmpty())
        return;
    QFile f(file);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(scriptText().toUtf8()) < 0)
        QMessageBox::warning(this, QStringLiteral("Save Script"),
                             QStringLiteral("Could not write %1").arg(file));
}
