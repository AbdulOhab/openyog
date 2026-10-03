#include "SqlDumpDialog.h"
#include "ConnectionParams.h" /* SqlDriverType */
#include "db/IDbConnection.h"

#include <QDateTime>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

SqlDumpDialog::SqlDumpDialog(IDbConnection *conn, const QString &db, const QStringList &preselect,
                             QWidget *parent)
    : QDialog(parent), m_conn(conn), m_db(db)
{
    setWindowTitle(QStringLiteral("Backup `%1` as SQL Dump").arg(db));

    m_path = new QLineEdit(m_db + QStringLiteral(".sql"), this);
    auto *browse = new QPushButton(QStringLiteral("&Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &SqlDumpDialog::browse);

    const bool isMy = conn && conn->driverType() == SqlDriverType::Mysql;
    const bool isLite = conn && conn->driverType() == SqlDriverType::Sqlite;

    m_tables = new QListWidget(this);
    m_tables->setMaximumHeight(220);
    const QStringList all =
        conn ? conn->listTables(db, QStringLiteral("BASE TABLE")) : QStringList();
    for(const QString &t : all) {
        auto *it = new QListWidgetItem(t, m_tables);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(preselect.isEmpty() || preselect.contains(t) ? Qt::Checked
                                                                       : Qt::Unchecked);
    }

    auto *mode = new QGroupBox(QStringLiteral("What to dump"), this);
    m_both = new QRadioButton(QStringLiteral("Structure and &data"), mode);
    m_both->setChecked(true);
    m_structOnly = new QRadioButton(QStringLiteral("Str&ucture only"), mode);
    m_dataOnly = new QRadioButton(QStringLiteral("&Data only"), mode);
    auto *modeLay = new QVBoxLayout(mode);
    modeLay->addWidget(m_both);
    modeLay->addWidget(m_structOnly);
    modeLay->addWidget(m_dataOnly);

    auto *opts = new QGroupBox(QStringLiteral("Options"), this);
    m_drops = new QCheckBox(QStringLiteral("Include \"DRO&P\" statements"), opts);
    m_drops->setChecked(true);
    m_routines = new QCheckBox(
        QStringLiteral("Include &routines (views/procedures/functions/triggers/events)"), opts);
    m_fkOff = new QCheckBox(QStringLiteral("Set FO&REIGN_KEY_CHECKS=0"), opts);
    m_fkOff->setChecked(true);
    m_singleTx = new QCheckBox(QStringLiteral("&Single transaction (consistent read view)"), opts);
    m_lockRead = new QCheckBox(QStringLiteral("Loc&k tables for read"), opts);
    m_flushLogs = new QCheckBox(QStringLiteral("Flush lo&gs before dump"), opts);
    m_lockInsert = new QCheckBox(QStringLiteral("Add lock around &INSERT statement(s)"), opts);
    m_useDb = new QCheckBox(QStringLiteral("Include \"&USE database\" statement"), opts);
    m_createDb = new QCheckBox(QStringLiteral("Include \"&CREATE database\" statement"), opts);
    m_blobHex = new QCheckBox(QStringLiteral("Convert &BLOB to HEX literals"), opts);
    m_bulk = new QCheckBox(QStringLiteral("Create &bulk INSERT statements"), opts);
    m_bulk->setChecked(true);
    m_rowsPer = new QSpinBox(opts);
    m_rowsPer->setRange(1, 100000);
    m_rowsPer->setValue(100);
    m_rowsPer->setLocale(QLocale::c());
    m_rowsPer->setEnabled(true);
    connect(m_bulk, &QCheckBox::toggled, m_rowsPer, &QSpinBox::setEnabled);

    /* MySQL-only statements; PG/SQLite get the reason in the tooltip */
    for(QCheckBox *c : {m_lockRead, m_flushLogs, m_lockInsert}) {
        c->setEnabled(isMy);
        c->setToolTip(isMy ? QString()
                           : QStringLiteral("MySQL only — this driver has no such statement"));
    }
    for(QCheckBox *c : {m_useDb, m_createDb}) {
        c->setEnabled(!isLite);
        c->setToolTip(isLite ? QStringLiteral("SQLite has no database/schema statement")
                             : QString());
    }

    auto *optsLay = new QVBoxLayout(opts);
    for(QWidget *w :
        std::initializer_list<QWidget *>{m_drops, m_routines, m_fkOff, m_singleTx, m_lockRead,
                                         m_flushLogs, m_lockInsert, m_useDb, m_createDb, m_blobHex})
        optsLay->addWidget(w);
    auto *bulkRow = new QHBoxLayout();
    bulkRow->addWidget(m_bulk);
    bulkRow->addWidget(new QLabel(QStringLiteral("rows:"), opts));
    bulkRow->addWidget(m_rowsPer);
    bulkRow->addStretch(1);
    optsLay->addLayout(bulkRow);

    m_filePerObject = new QCheckBox(QStringLiteral("&File per object (one file per table)"), this);
    m_timestamp = new QCheckBox(QStringLiteral("Pre&fix filename with timestamp"), this);
    m_pathHint = new QLabel(this);
    connect(m_filePerObject, &QCheckBox::toggled, this, [this](bool on) {
        m_pathHint->setText(on ? QStringLiteral("Target is a directory; each table gets "
                                                "<table>.sql inside it")
                               : QString());
    });

    auto *pathRow = new QHBoxLayout();
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browse);

    auto *left = new QVBoxLayout();
    left->addWidget(new QLabel(QStringLiteral("Tables (all, when none checked):"), this));
    left->addWidget(m_tables, 1);

    auto *right = new QVBoxLayout();
    right->addWidget(mode);
    right->addWidget(opts, 1);

    auto *columns = new QHBoxLayout();
    columns->addLayout(left, 2);
    columns->addLayout(right, 3);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("&Dump…"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(QStringLiteral("Export to &file/directory:"), this));
    lay->addLayout(pathRow);
    lay->addWidget(m_pathHint);
    lay->addWidget(m_filePerObject);
    lay->addWidget(m_timestamp);
    lay->addLayout(columns);
    lay->addWidget(bb);
    resize(760, 520);
}

void SqlDumpDialog::browse()
{
    if(m_filePerObject->isChecked()) {
        const QString dir =
            QFileDialog::getExistingDirectory(this, QStringLiteral("Export to directory"));
        if(!dir.isEmpty())
            m_path->setText(dir);
    } else {
        const QString f =
            QFileDialog::getSaveFileName(this, QStringLiteral("Backup as SQL dump"), m_path->text(),
                                         QStringLiteral("SQL (*.sql);;All (*)"));
        if(!f.isEmpty())
            m_path->setText(f);
    }
}

QStringList SqlDumpDialog::selectedTables() const
{
    QStringList picked;
    for(int i = 0; i < m_tables->count(); ++i)
        if(m_tables->item(i)->checkState() == Qt::Checked)
            picked << m_tables->item(i)->text();
    if(picked.size() == m_tables->count())
        picked.clear(); /* all → let SqlDump enumerate */
    return picked;
}

SqlDump::Options SqlDumpDialog::options() const
{
    SqlDump::Options opt;
    opt.structure = !m_dataOnly->isChecked();
    opt.data = !m_structOnly->isChecked();
    opt.addDropTable = m_drops->isChecked();
    opt.routines = m_routines->isChecked();
    opt.rowsPerInsert = m_bulk->isChecked() ? m_rowsPer->value() : 1;
    opt.fkChecksOff = m_fkOff->isChecked();
    opt.singleTransaction = m_singleTx->isChecked();
    opt.lockTablesForRead = m_lockRead->isEnabled() && m_lockRead->isChecked();
    opt.flushLogs = m_flushLogs->isEnabled() && m_flushLogs->isChecked();
    opt.lockAroundInsert = m_lockInsert->isEnabled() && m_lockInsert->isChecked();
    opt.includeUseDb = m_useDb->isEnabled() && m_useDb->isChecked();
    opt.includeCreateDb = m_createDb->isEnabled() && m_createDb->isChecked();
    opt.blobToHex = m_blobHex->isChecked();
    return opt;
}

QString SqlDumpDialog::targetPath() const
{
    QString p = m_path->text().trimmed();
    if(!m_filePerObject->isChecked() && m_timestamp->isChecked() && !p.isEmpty()) {
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmm"));
        const int dot = p.lastIndexOf(QLatin1Char('.'));
        if(dot > p.lastIndexOf(QLatin1Char('/')))
            p = p.left(dot) + QLatin1Char('-') + stamp + p.mid(dot);
        else
            p += QLatin1Char('-') + stamp;
    }
    return p;
}

bool SqlDumpDialog::filePerObject() const
{
    return m_filePerObject->isChecked();
}

QString SqlDumpDialog::timestampPrefix() const
{
    return m_timestamp->isChecked()
               ? QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmm")) +
                     QLatin1Char('-')
               : QString();
}
