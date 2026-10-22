/* OpenYog — the Database Synchronization Wizard; see DataSyncDialog.h. */
#include "DataSyncDialog.h"
#include "Icons.h"
#include "db/IDbConnection.h"
#include "db/IDbDriver.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QThreadPool>
#include <QTreeWidget>
#include <QVBoxLayout>

using DataSync::TableResult;

namespace {

enum TreeColumn { CTable, CInsert, CUpdate, CDelete, CNote };

/* the editor isn't for a script bigger than this; Save Script is */
constexpr qsizetype kEditorLimit = 8 * 1024 * 1024;

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

QString rows(qint64 n)
{
    return QLocale().toString(n);
}

} // namespace

DataSyncDialog::DataSyncDialog(QList<Endpoint> endpoints, QWidget *parent)
    : QDialog(parent), m_endpoints(std::move(endpoints))
{
    setWindowTitle(QStringLiteral("Data Synchronization"));
    setWindowIcon(Icons::get(QStringLiteral("dbsync_16.ico")));
    resize(900, 600);

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
    auto *sourceBox = side(QStringLiteral("Source: the rows to copy"), m_sourceConn, m_sourceDbBox);
    auto *targetBox =
        side(QStringLiteral("Target: the database to change"), m_targetConn, m_targetDbBox);
    auto *arrow = new QLabel(QStringLiteral("→"), this);
    arrow->setStyleSheet(QStringLiteral("font-size: 20pt;"));
    m_compare = new QPushButton(QStringLiteral("&Compare"), this);
    m_compare->setDefault(true);
    connect(m_compare, &QPushButton::clicked, this, &DataSyncDialog::compareNow);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancelButton->hide();
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        if(m_cancel)
            *m_cancel = true;
    });
    auto *buttonsColumn = new QVBoxLayout;
    buttonsColumn->addStretch();
    buttonsColumn->addWidget(m_compare);
    buttonsColumn->addWidget(m_cancelButton);
    auto *sides = new QHBoxLayout;
    sides->addWidget(sourceBox, 1);
    sides->addWidget(arrow);
    sides->addWidget(targetBox, 1);
    sides->addLayout(buttonsColumn);

    m_status = new QLabel(QStringLiteral("Pick the two databases and compare their rows. "
                                         "Nothing is changed here: the script is yours to run."),
                          this);
    m_status->setWordWrap(true);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({QStringLiteral("Table"), QStringLiteral("Insert"),
                             QStringLiteral("Update"), QStringLiteral("Delete"),
                             QStringLiteral("Note")});
    m_tree->setRootIsDecorated(false);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(CTable, 220);
    for(int c : {CInsert, CUpdate, CDelete})
        m_tree->setColumnWidth(c, 120);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this] { updateButtons(); });

    auto *buttons = new QDialogButtonBox(this);
    m_open = buttons->addButton(QStringLiteral("Open in Target's &Query Editor"),
                                QDialogButtonBox::ActionRole);
    m_save = buttons->addButton(QStringLiteral("&Save Script…"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &DataSyncDialog::reject);
    connect(m_open, &QPushButton::clicked, this, [this] { writeScript({}); });
    connect(m_save, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Script"),
            QStringLiteral("data-sync-%1-to-%2.sql").arg(m_sourceDb, m_targetDb),
            QStringLiteral("SQL (*.sql)"));
        if(!file.isEmpty())
            writeScript(file);
    });

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(sides);
    lay->addWidget(m_status);
    lay->addWidget(m_tree, 1);
    lay->addWidget(buttons);

    fillDatabases(m_sourceDbBox, 0, m_endpoints.value(0).currentDatabase());
    const int target = m_endpoints.size() > 1 ? 1 : 0;
    m_targetConn->setCurrentIndex(target);
    fillDatabases(m_targetDbBox, target, m_endpoints.value(target).currentDatabase());
    updateButtons();
}

DataSyncDialog::~DataSyncDialog()
{
    if(m_cancel)
        *m_cancel = true; /* a worker still running stops at its next row */
}

void DataSyncDialog::reject()
{
    if(m_cancel)
        *m_cancel = true;
    QDialog::reject();
}

void DataSyncDialog::fillDatabases(QComboBox *databases, int endpoint, const QString &select)
{
    databases->clear();
    if(endpoint < 0 || endpoint >= m_endpoints.size())
        return;
    databases->addItems(m_endpoints[endpoint].databases());
    const int at = databases->findText(select);
    if(at >= 0)
        databases->setCurrentIndex(at);
}

void DataSyncDialog::compareForTest(int source, const QString &sourceDb, int target,
                                    const QString &targetDb)
{
    m_sourceConn->setCurrentIndex(source);
    fillDatabases(m_sourceDbBox, source, sourceDb);
    m_targetConn->setCurrentIndex(target);
    fillDatabases(m_targetDbBox, target, targetDb);
    compareNow();
    QElapsedTimer waited;
    waited.start();
    while(m_busy && waited.elapsed() < 60000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

QStringList DataSyncDialog::shownForTest() const
{
    QStringList out{m_status->text()};
    for(int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = m_tree->topLevelItem(i);
        QStringList cells;
        for(int c = CTable; c <= CNote; ++c) {
            QString cell = item->text(c);
            if(!item->data(c, Qt::CheckStateRole).isNull())
                cell.prepend(item->checkState(c) == Qt::Checked ? "[x] " : "[ ] ");
            cells << cell;
        }
        out << cells.join(QStringLiteral(" | "));
    }
    return out;
}

void DataSyncDialog::setBusy(bool busy, const QString &status)
{
    m_busy = busy;
    m_compare->setEnabled(!busy);
    m_cancelButton->setVisible(busy);
    m_tree->setEnabled(!busy);
    if(!status.isEmpty())
        m_status->setText(status);
    updateButtons();
}

void DataSyncDialog::updateButtons()
{
    const bool any = !m_busy && !picks().isEmpty();
    m_open->setEnabled(any);
    m_save->setEnabled(any);
}

void DataSyncDialog::compareNow()
{
    const int s = m_sourceConn->currentIndex(), t = m_targetConn->currentIndex();
    if(s < 0 || t < 0 || m_busy)
        return;
    m_tree->clear();
    m_results.clear();
    m_targetEndpoint = -1;
    if(m_endpoints[s].driver != m_endpoints[t].driver) {
        m_status->setText(
            QStringLiteral("The source is %1 and the target %2: Data Sync compares "
                           "two databases of the same kind.")
                .arg(driverName(m_endpoints[s].driver), driverName(m_endpoints[t].driver)));
        updateButtons();
        return;
    }
    m_sourceDb = m_sourceDbBox->currentText();
    m_targetDb = m_targetDbBox->currentText();
    if(s == t && m_sourceDb == m_targetDb) {
        m_status->setText(QStringLiteral("The source and the target are the same database."));
        updateButtons();
        return;
    }
    m_sourceParams = m_endpoints[s].params(m_sourceDb);
    m_targetParams = m_endpoints[t].params(m_targetDb);
    m_targetEndpoint = t;

    setBusy(true, QStringLiteral("Connecting…"));
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    const QPointer<DataSyncDialog> guard(this);
    const ConnectionParams sp = m_sourceParams, tp = m_targetParams;
    const QString sdb = m_sourceDb, tdb = m_targetDb;
    QThreadPool::globalInstance()->start([guard, cancel, sp, tp, sdb, tdb] {
        QString error;
        std::unique_ptr<IDbConnection> source(dbDriverFor(sp.driverType)->connect(sp, &error));
        std::unique_ptr<IDbConnection> target(
            source ? dbDriverFor(tp.driverType)->connect(tp, &error) : nullptr);
        QList<TableResult> results;
        if(source && target) {
            const QStringList tables = DataSync::commonTables(source.get(), sdb, target.get(), tdb);
            for(int i = 0; i < tables.size() && !*cancel; ++i) {
                QMetaObject::invokeMethod(
                    guard,
                    [guard, i, n = tables.size(), name = tables[i]] {
                        if(guard)
                            guard->m_status->setText(QStringLiteral("Comparing %1 (%2 of %3)…")
                                                         .arg(name)
                                                         .arg(i + 1)
                                                         .arg(n));
                    },
                    Qt::QueuedConnection);
                results << DataSync::compareTable(source.get(), sdb, target.get(), tdb, tables[i],
                                                  {}, 100, [cancel] { return cancel->load(); });
            }
        }
        const bool cancelled = *cancel;
        QMetaObject::invokeMethod(
            guard,
            [guard, results, error, cancelled] {
                if(!guard)
                    return;
                guard->setBusy(false);
                if(cancelled) {
                    guard->m_status->setText(QStringLiteral("Cancelled."));
                    return;
                }
                if(!error.isEmpty()) {
                    guard->m_status->setText(QStringLiteral("Could not connect: %1").arg(error));
                    return;
                }
                guard->showResults(results);
            },
            Qt::QueuedConnection);
    });
}

void DataSyncDialog::showResults(const QList<TableResult> &results)
{
    m_results = results;
    m_tree->blockSignals(true);
    qint64 inserts = 0, updates = 0, deletes = 0;
    int differing = 0;
    for(const TableResult &r : results) {
        auto *item = new QTreeWidgetItem(m_tree, {r.table});
        item->setIcon(CTable, Icons::get(QStringLiteral("table.ico")));
        if(!r.skipped.isEmpty()) {
            item->setText(CNote, r.skipped);
            for(int c = CTable; c <= CNote; ++c)
                item->setForeground(c, palette().color(QPalette::Disabled, QPalette::Text));
            continue;
        }
        const bool differs = r.inserts + r.updates + r.deletes > 0;
        const auto kind = [&](int column, qint64 n, const QString &verb, bool picked) {
            if(n == 0) {
                item->setText(column, QStringLiteral("—"));
                return;
            }
            item->setText(column, QStringLiteral("%1 to %2").arg(rows(n), verb));
            item->setCheckState(column, picked ? Qt::Checked : Qt::Unchecked);
        };
        kind(CInsert, r.inserts, QStringLiteral("insert"), true);
        kind(CUpdate, r.updates, QStringLiteral("update"), true);
        kind(CDelete, r.deletes, QStringLiteral("delete"), false);
        if(differs) {
            item->setCheckState(CTable, Qt::Checked);
            ++differing;
        }
        QStringList note;
        if(!differs)
            note << QStringLiteral("the same %1 rows").arg(rows(r.same));
        if(!r.ignored.isEmpty())
            note << QStringLiteral("not compared, in one table only: %1")
                        .arg(r.ignored.join(QStringLiteral(", ")));
        item->setText(CNote, note.join(QStringLiteral("; ")));
        inserts += r.inserts;
        updates += r.updates;
        deletes += r.deletes;
    }
    m_tree->blockSignals(false);
    m_status->setText(
        differing == 0
            ? QStringLiteral("The rows of the %1 tables both databases have match.")
                  .arg(results.size())
            : QStringLiteral("%1 of %2 tables differ: %3 rows to insert, %4 to update, %5 to "
                             "delete. Deletes start unticked.")
                  .arg(differing)
                  .arg(results.size())
                  .arg(rows(inserts), rows(updates), rows(deletes)));
    updateButtons();
}

QList<DataSync::Pick> DataSyncDialog::picks() const
{
    QList<DataSync::Pick> out;
    for(int i = 0; i < m_tree->topLevelItemCount() && i < m_results.size(); ++i) {
        const QTreeWidgetItem *item = m_tree->topLevelItem(i);
        if(item->data(CTable, Qt::CheckStateRole).isNull() ||
           item->checkState(CTable) != Qt::Checked)
            continue;
        const auto on = [item](int column) {
            return !item->data(column, Qt::CheckStateRole).isNull() &&
                   item->checkState(column) == Qt::Checked;
        };
        DataSync::Pick p{m_results[i].table, on(CInsert), on(CUpdate), on(CDelete)};
        if(p.inserts || p.updates || p.deletes)
            out << p;
    }
    return out;
}

void DataSyncDialog::writeScript(const QString &file)
{
    const QList<DataSync::Pick> chosen = picks();
    if(chosen.isEmpty() || m_busy)
        return;
    setBusy(true, QStringLiteral("Writing the script…"));
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    const QPointer<DataSyncDialog> guard(this);
    const ConnectionParams sp = m_sourceParams, tp = m_targetParams;
    const QString sdb = m_sourceDb, tdb = m_targetDb;
    QThreadPool::globalInstance()->start([guard, cancel, sp, tp, sdb, tdb, chosen, file] {
        QString error;
        std::unique_ptr<IDbConnection> source(dbDriverFor(sp.driverType)->connect(sp, &error));
        std::unique_ptr<IDbConnection> target(
            source ? dbDriverFor(tp.driverType)->connect(tp, &error) : nullptr);
        QByteArray script;
        qint64 written = 0;
        bool tooBig = false, ok = false;
        if(source && target) {
            QFile out(file);
            if(!file.isEmpty() && !out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                error = QStringLiteral("Could not write %1").arg(file);
            } else {
                ok = DataSync::writeScript(
                    source.get(), sdb, target.get(), tdb, chosen,
                    [&](const QByteArray &part) {
                        written += part.size();
                        if(!file.isEmpty())
                            out.write(part);
                        else if(script.size() + part.size() > kEditorLimit)
                            tooBig = true;
                        else
                            script += part;
                    },
                    [&] { return cancel->load() || tooBig; }, &error);
            }
        }
        const bool cancelled = *cancel;
        QMetaObject::invokeMethod(
            guard,
            [guard, script, written, tooBig, ok, error, cancelled, file] {
                if(!guard)
                    return;
                guard->setBusy(false);
                if(cancelled) {
                    guard->m_status->setText(QStringLiteral("Cancelled."));
                } else if(tooBig) {
                    guard->m_status->setText(QStringLiteral("The script is over %1 MB, too big for "
                                                            "the query editor: save it instead.")
                                                 .arg(kEditorLimit / (1024 * 1024)));
                } else if(!ok) {
                    guard->m_status->setText(error);
                } else if(file.isEmpty()) {
                    const Endpoint &e = guard->m_endpoints[guard->m_targetEndpoint];
                    if(e.openInEditor)
                        e.openInEditor(QString::fromUtf8(script));
                    guard->m_status->setText(QStringLiteral("The script is in the target's query "
                                                            "editor."));
                } else {
                    guard->m_status->setText(QStringLiteral("Saved %1 (%2 KB).")
                                                 .arg(file)
                                                 .arg(QLocale().toString(written / 1024)));
                }
            },
            Qt::QueuedConnection);
    });
}
