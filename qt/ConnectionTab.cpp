#include "ConnectionTab.h"
#include "ObjectBrowser.h"
#include "RecentFiles.h"
#include "TableDataView.h"

#include "CreateTableDialog.h"
#include "ExportDialog.h"
#include "ResultExport.h"
#include "SqlSplit.h"
#include "FindBar.h"
#include "InfoPanel.h"
#include "SqlFormat.h"
#include "UserManagerDialog.h"
#include "IndexDialog.h"
#include "ForeignKeyDialog.h"
#include "SqlDump.h"
#include "SqlDumpDialog.h"
#include "FavoritesStore.h"
#include "Icons.h"
#include "wyIni.h"
#include "db/IDbDriver.h"
#include "db/IDbConnection.h"

#include <QApplication>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QLineEdit>
#include <QFontDatabase>
#include <QTextStream>
#include <QHeaderView>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSplitter>
#include <QTabBar>
#include <QScrollBar>
#include <QTextBrowser>
#include <QUrl>
#include <QToolButton>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QTime>
#include <QVBoxLayout>
#include <utility>

#include <QClipboard>
#include <QKeySequence>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QMutex>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>

/* guards {live, mutex} below: the worker thread sets/clears `live` while
 * holding `mutex`, and ConnectionTab::cancelQuery() (GUI thread) reads it
 * and calls cancel() while holding the SAME lock — so a cancel() call is
 * either fully serialized before the clear (connection still valid,
 * cancel() runs normally) or fully after it (sees null, no-ops); the
 * connection can never be deleted while a concurrent cancel() call is in
 * progress. Declared at file scope (matching the forward declaration in
 * ConnectionTab.h) rather than in the anonymous namespace below, since an
 * anonymous-namespace type can't be named from the header. */
struct LiveConnection
{
    QMutex mutex;
    IDbConnection *live = nullptr;
};

namespace {

/* query timeout: same OpenYog.ini Theme::load()/save() already use, a new
 * [Query] section. 0 = disabled (the default — no upstream equivalent to
 * default to, and silently killing a long-running statement would surprise
 * a user who never asked for a limit). */
QString settingsIniPath()
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    dir.mkpath(".");
    return dir.filePath("OpenYog.ini");
}

int loadQueryTimeoutSecs()
{
    return wyIni::IniGetInt("Query", "timeout_secs", 0, settingsIniPath().toUtf8());
}

void saveQueryTimeoutSecs(int secs)
{
    wyIni::IniWriteInt("Query", "timeout_secs", secs, settingsIniPath().toUtf8());
}

/* persistent query history: <AppConfig>/history.log, tab-separated
 * "<iso datetime>\t<connection name>\t<sql, newlines flattened>" */
QString historyPath()
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    dir.mkpath(QStringLiteral("."));
    return dir.filePath(QStringLiteral("history.log"));
}

void appendHistoryLine(const QString &conn, const QString &sql)
{
    QFile f(historyPath());
    if(!f.open(QIODevice::Append | QIODevice::Text))
        return;
    QString one = sql;
    one.replace('\n', QLatin1Char(' ')).replace('\r', QString());
    QTextStream(&f) << QDateTime::currentDateTime().toString(Qt::ISODate) << '\t' << conn << '\t'
                    << one << '\n';
}

/* the last `max` history lines for `conn`, oldest first, as "[time date] sql" */
/* {display line "[ts] sql", raw query} pairs for `conn`, oldest first */
QList<QPair<QString, QString>> loadHistoryFor(const QString &conn, int max)
{
    QFile f(historyPath());
    if(!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QList<QPair<QString, QString>> out;
    QTextStream in(&f);
    while(!in.atEnd()) {
        const QStringList p = in.readLine().split('\t');
        if(p.size() < 3 || p[1] != conn)
            continue;
        const QDateTime dt = QDateTime::fromString(p[0], Qt::ISODate);
        const QString sql = p.mid(2).join(QLatin1Char('\t'));
        out << qMakePair(
            QStringLiteral("[%1] %2").arg(
                dt.isValid() ? dt.toString(QStringLiteral("MMM d  hh:mm:ss")) : p[0], sql),
            sql);
    }
    return out.mid(qMax(0, out.size() - max));
}

/* result-grid sort: numeric when both cells parse as numbers, NULL last */
class GridSortProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool lessThan(const QModelIndex &l, const QModelIndex &r) const override
    {
        const QString a = sourceModel()->data(l).toString();
        const QString b = sourceModel()->data(r).toString();
        if(a == QStringLiteral("NULL") || b == QStringLiteral("NULL"))
            return a != QStringLiteral("NULL"); /* NULLs sort to the end */
        bool an = false, bn = false;
        const double av = a.toDouble(&an), bv = b.toDouble(&bn);
        if(an && bn)
            return av < bv;
        return QString::localeAwareCompare(a, b) < 0;
    }
};

/* Ctrl+C on a QTableView → TSV of the selected block onto the clipboard */
void installGridCopy(QTableView *grid)
{
    auto *sc = new QShortcut(QKeySequence::Copy, grid);
    QObject::connect(sc, &QShortcut::activated, grid, [grid] {
        const QModelIndexList sel = grid->selectionModel()->selectedIndexes();
        if(sel.isEmpty())
            return;
        int r0 = sel.first().row(), r1 = r0, c0 = sel.first().column(), c1 = c0;
        for(const QModelIndex &i : sel) {
            r0 = qMin(r0, i.row());
            r1 = qMax(r1, i.row());
            c0 = qMin(c0, i.column());
            c1 = qMax(c1, i.column());
        }
        QString out;
        for(int r = r0; r <= r1; ++r) {
            QStringList cells;
            for(int c = c0; c <= c1; ++c)
                cells << grid->model()->index(r, c).data().toString();
            out += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
        }
        QApplication::clipboard()->setText(out);
    });
}

/* runs in a worker thread: dedicated connection per batch, results
 * collected as plain data (no driver objects cross threads) */
QVector<QueryResult> runOnConnection(const ConnectionParams &p, const QStringList &statements,
                                     LiveConnection *liveConn = nullptr)
{
    QVector<QueryResult> results;
    QString error;
    IDbConnection *c = dbDriverFor(p.driverType)->connect(p, &error);
    if(!c) {
        QueryResult r;
        r.ok = false;
        r.message = error;
        results.append(r);
        return results;
    }
    if(liveConn) {
        QMutexLocker lock(&liveConn->mutex);
        liveConn->live = c;
    }

    for(const QString &stmt : statements) {
        QueryResult r;
        QElapsedTimer timer;
        timer.start();

        DbResultSet rs;
        QString message;
        r.ok = c->query(stmt, &rs, &message);
        if(r.ok) {
            r.headers = rs.headers;
            r.rows = rs.rows;
            r.message = rs.headers.isEmpty()
                            ? QStringLiteral("OK, %1 row(s) affected").arg(c->affectedRows())
                            : QStringLiteral("%1 row(s)").arg(r.rows.size());
        } else {
            r.message = message;
        }
        r.secs = timer.elapsed() / 1000.0;
        results.append(r);
    }

    if(liveConn) {
        QMutexLocker lock(&liveConn->mutex);
        liveConn->live = nullptr;
    }
    delete c;
    return results;
}

} // namespace

ConnectionTab::ConnectionTab(const ConnectionParams &params, QWidget *parent)
    : QWidget(parent), m_params(params), m_cancelState(std::make_shared<LiveConnection>())
{
    /* ---- left: object browser ------------------------------------ */
    m_browser = new ObjectBrowser(this);
    m_browser->setConnectionResolver(
        [this](const QString &database, QString *error) { return connectionFor(database, error); });
    connect(m_browser, &ObjectBrowser::openDatabaseInNewTabRequested, this,
            [this](const QString &database) { emit newTabRequested(paramsFor(database)); });
    connect(m_browser, &ObjectBrowser::switchDatabaseRequested, this,
            &ConnectionTab::switchDatabase);

    /* ---- right-top: editor tabs (Query 1 / History) --------------- */
    m_editor = new SqlEditor(this);
    attachEditor(m_editor, QStringLiteral("Query 1"));

    m_history = new QTextBrowser(this);
    m_history->setOpenLinks(false);
    m_history->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    connect(m_history, &QTextBrowser::anchorClicked, this, [this](const QUrl &u) {
        const int i = u.path().toInt();
        if(i < 0 || i >= m_historyQueries.size() || m_historyQueries[i].isEmpty())
            return;
        if(u.scheme() == QStringLiteral("c"))
            QApplication::clipboard()->setText(m_historyQueries[i]);
        else
            sendHistoryToEditor(SqlFormat::pretty(m_historyQueries[i]));
    });
    /* previous sessions' history for this connection */
    for(const auto &pr : loadHistoryFor(params.name, 500)) {
        m_historyLines << pr.first;
        m_historyQueries << pr.second;
    }
    if(!m_historyLines.isEmpty()) {
        m_historyLines << QStringLiteral("——— this session ———");
        m_historyQueries << QString();
    }

    m_historySearch = new QLineEdit(this);
    m_historySearch->setPlaceholderText(QStringLiteral("filter history…"));
    connect(m_historySearch, &QLineEdit::textChanged, this, &ConnectionTab::renderHistory);
    /* explicit reset button — the built-in clear ✕ is unreliable under the
     * app stylesheet / icon theme */
    auto *histReset = new QToolButton(this);
    histReset->setText(QStringLiteral("✕"));
    histReset->setAutoRaise(true);
    histReset->setToolTip(QStringLiteral("Clear the filter"));
    connect(histReset, &QToolButton::clicked, m_historySearch, &QLineEdit::clear);
    /* explicit label + ellipsis: this wipes the saved log, not the filter box */
    auto *histClear = new QPushButton(QStringLiteral("Clear History…"), this);
    histClear->setToolTip(QStringLiteral("Delete the saved query history file"));
    connect(histClear, &QPushButton::clicked, this, &ConnectionTab::clearHistory);
    auto *histCopyAll = new QPushButton(QStringLiteral("Copy All"), this);
    histCopyAll->setToolTip(
        QStringLiteral("Copy every query shown (matching the filter) as one script"));
    connect(histCopyAll, &QPushButton::clicked, this, &ConnectionTab::copyAllShownHistory);
    auto *histTop = new QHBoxLayout;
    histTop->setContentsMargins(3, 3, 3, 0);
    histTop->addWidget(new QLabel(QStringLiteral("Filter:"), this));
    histTop->addWidget(m_historySearch, 1);
    histTop->addWidget(histReset);
    histTop->addSpacing(8);
    histTop->addWidget(histCopyAll);
    histTop->addWidget(histClear);
    m_historyPage = new QWidget(this);
    auto *histCol = new QVBoxLayout(m_historyPage);
    histCol->setContentsMargins(0, 0, 0, 0);
    histCol->setSpacing(2);
    histCol->addLayout(histTop);
    histCol->addWidget(m_history, 1);
    renderHistory();

    m_editorTabs = new QTabWidget(this);
    m_editorTabs->setObjectName(QStringLiteral("editorTabs"));
    m_editorTabs->setDocumentMode(true);
    m_editorTabs->setTabsClosable(true);
    m_editorTabs->setMovable(true);
    m_editorTabs->tabBar()->setExpanding(false); /* SQLyog left-aligns tabs */
    connect(m_editorTabs, &QTabWidget::tabCloseRequested, this, &ConnectionTab::closeEditorTab);
    m_editorTabs->setCornerWidget(
        [&] {
            auto *plus = new QPushButton(QStringLiteral("+"), this);
            plus->setFlat(true);
            plus->setFixedSize(22, 20);
            plus->setStyleSheet(
                QStringLiteral("QPushButton { background: transparent; color: #3B7DBB; "
                               "border: none; font-weight: bold; }"
                               "QPushButton:hover { background: #E8F2FA; }"));
            connect(plus, &QPushButton::clicked, this, &ConnectionTab::addEditorTab);
            return plus;
        }(),
        Qt::TopRightCorner);
    m_editorTabs->addTab(m_editor, Icons::get(QStringLiteral("query_16.ico")),
                         QStringLiteral("Query 1"));
    m_editorTabs->addTab(m_historyPage, Icons::get(QStringLiteral("history.ico")),
                         QStringLiteral("History"));

    /* ---- right-bottom: result tabs -------------------------------- */
    m_messages = new QPlainTextEdit(this);
    m_messages->setReadOnly(true);

    m_info = new InfoPanel(this);

    m_resultTabs = new QTabWidget(this);
    m_resultTabs->setObjectName(QStringLiteral("resultTabs"));
    m_resultTabs->setDocumentMode(true);
    m_resultTabs->setTabPosition(QTabWidget::North);
    m_resultTabs->tabBar()->setExpanding(false); /* SQLyog left-aligns tabs */
    /* the three fixed tabs below stay open always (no × of their own —
     * hidden right after they're added); each query run instead adds its
     * own new "Execute Query N" tab here, closable independently, so
     * running another query never throws away the previous result */
    m_resultTabs->setTabsClosable(true);
    connect(m_resultTabs, &QTabWidget::tabCloseRequested, this, &ConnectionTab::closeResultTab);
    m_tableData = new TableDataView(this);

    m_resultTabs->addTab(m_messages, Icons::get(QStringLiteral("notification.ico")),
                         QStringLiteral("Messages"));
    m_resultTabs->addTab(m_tableData, Icons::get(QStringLiteral("grid_view.ico")),
                         QStringLiteral("Table Data"));
    m_resultTabs->addTab(m_info, Icons::get(QStringLiteral("info.ico")), QStringLiteral("Info"));
    for(int i = 0; i < 3; ++i)
        m_resultTabs->tabBar()->setTabButton(i, QTabBar::RightSide, nullptr);
    m_resultTabs->setCurrentIndex(0);
    /* the connection label tracks which result page is in front: a side
     * connection is only "the active connection" while its Table Data
     * grid is the visible one (see m_tableDataPhysDb) */
    connect(m_resultTabs, &QTabWidget::currentChanged, this,
            [this](int) { updateActiveConnectionLabel(); });

    m_findBar = new FindBar([this] { return currentEditor(); }, this);

    auto *editorSide = new QWidget(this);
    auto *editorCol = new QVBoxLayout(editorSide);
    editorCol->setContentsMargins(0, 0, 0, 0);
    editorCol->setSpacing(0);
    editorCol->addWidget(m_editorTabs, 1);
    editorCol->addWidget(m_findBar);

    auto *rightSplit = new QSplitter(Qt::Vertical, this);
    rightSplit->addWidget(editorSide);
    rightSplit->addWidget(m_resultTabs);
    rightSplit->setStretchFactor(0, 1);
    rightSplit->setStretchFactor(1, 1);
    rightSplit->setSizes({360, 360});

    auto *mainSplit = new QSplitter(Qt::Horizontal, this);
    mainSplit->addWidget(m_browser);
    mainSplit->addWidget(rightSplit);
    mainSplit->setStretchFactor(0, 0);
    mainSplit->setStretchFactor(1, 1);
    mainSplit->setSizes({215, 985}); /* spec: object browser ~1/5 width */

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(mainSplit, 1);

    /* ---- open the connection ------------------------------------- */
    {
        QString error;
        m_conn = dbDriverFor(m_params.driverType)->connect(m_params, &error, /*localInfile=*/true);
        if(!m_conn) {
            m_messages->setPlainText(QStringLiteral("Connection failed: ") + error);
            return;
        }
    }

    /* Connect dialog's "Keep-Alive Interval": a periodic no-op query on the
     * browsing connection so a firewall/proxy — or the server's own idle
     * timeout — doesn't drop it while the user is just reading, not typing.
     * Skipped while a batch is running: it shares no state with the worker
     * thread's own connection, but there's no reason to add an extra query
     * on top of one already in flight. SQLite is a local file, not a
     * server socket that can time out — nothing to keep alive. */
    if(m_params.driverType == SqlDriverType::Mysql && m_params.keepAliveSecs > 0) {
        m_keepAliveTimer = new QTimer(this);
        m_keepAliveTimer->setInterval(m_params.keepAliveSecs * 1000);
        connect(m_keepAliveTimer, &QTimer::timeout, this, [this] {
            if(m_conn && !m_running)
                m_conn->query(QStringLiteral("SELECT 1"), nullptr, nullptr);
        });
        m_keepAliveTimer->start();
    }

    /* a click shows the object in Info AND — for a table — loads its
     * Table Data grid immediately, so both right-pane tabs sit on the
     * same object at the same time (owner directive: the old lazy path
     * parked the click in m_pendingTable and only loaded the grid if
     * Table Data happened to be in front, leaving Info on the new table
     * while the grid still showed the previous one — "আলাদা লোড").
     * Two guards survive from the old flow: never reload the table the
     * grid already shows (keeps its sort/filter) and never clobber
     * staged grid edits with a browse click. */
    connect(m_browser, &ObjectBrowser::objectSelected, this,
            [this](const QString &db, const QString &objType, const QString &name,
                   const QString &physDb) {
                updateInfoTab(db, objType, name, physDb);
                if(objType != QStringLiteral("TABLE"))
                    return;
                if(m_tableData->hasStagedEdits())
                    return;
                if(QStringList{physDb, db, name}.join(QLatin1Char('\x1f')) == m_loadedTableKey)
                    return;
                loadTableData(db, name, physDb, false);
            });
    connect(m_browser, &ObjectBrowser::insertNameRequested, this, [this](const QString &text) {
        SqlEditor *ed = currentEditor();
        if(!ed)
            return;
        /* SCI_REPLACESEL, like upstream: unlike QsciScintilla::insert() it
         * leaves the caret after the inserted name, not in front of it */
        ed->replaceSelectedText(text);
        ed->setFocus();
    });
    connect(m_browser, &ObjectBrowser::databaseActivated, this, &ConnectionTab::useDatabase);
    connect(m_browser, &ObjectBrowser::tableActivated, this,
            [this](const QString &db, const QString &table, const QString &physDb) {
                loadTableData(db, table, physDb, true);
            });
    connect(m_tableData, &TableDataView::statusMessage, this,
            [this](const QString &text) { m_messages->appendPlainText(text); });

    connect(m_browser, &ObjectBrowser::dropTableRequested, this, &ConnectionTab::dropTable);
    connect(m_browser, &ObjectBrowser::createTableRequested, this,
            [this](const QString &db) { promptCreateTable(db); });
    connect(m_browser, &ObjectBrowser::alterTableRequested, this,
            [this](const QString &db, const QString &table) { promptAlterTable(db, table); });
    connect(m_browser, &ObjectBrowser::renameTableRequested, this,
            &ConnectionTab::promptRenameTable);
    connect(m_browser, &ObjectBrowser::copyTableRequested, this, &ConnectionTab::promptCopyTable);
    connect(m_browser, &ObjectBrowser::manageIndexesRequested, this,
            &ConnectionTab::promptManageIndexes);
    connect(m_browser, &ObjectBrowser::manageForeignKeysRequested, this,
            &ConnectionTab::promptManageForeignKeys);
    connect(m_browser, &ObjectBrowser::dumpDatabaseRequested, this,
            [this](const QString &db) { promptDumpDatabase(db); });
    connect(m_browser, &ObjectBrowser::copyDatabaseRequested, this,
            [this](const QString &db) { promptCopyDatabase(db); });
    connect(m_browser, &ObjectBrowser::importCsvRequested, this, &ConnectionTab::promptImportCsv);
    connect(m_browser, &ObjectBrowser::importXmlRequested, this, &ConnectionTab::promptImportXml);
    connect(m_browser, &ObjectBrowser::exportTableRequested, this, &ConnectionTab::exportTableData);
    connect(m_browser, &ObjectBrowser::truncateTableRequested, this, &ConnectionTab::truncateTable);
    connect(m_browser, &ObjectBrowser::createObjectRequested, this,
            &ConnectionTab::createSchemaObject);
    connect(m_browser, &ObjectBrowser::alterObjectRequested, this,
            &ConnectionTab::alterSchemaObject);
    connect(m_browser, &ObjectBrowser::dropObjectRequested, this, &ConnectionTab::dropSchemaObject);
    connect(m_browser, &ObjectBrowser::dropDatabaseRequested, this, &ConnectionTab::dropDatabase);
    connect(m_browser, &ObjectBrowser::truncateDatabaseRequested, this,
            &ConnectionTab::truncateDatabase);
    connect(m_browser, &ObjectBrowser::emptyDatabaseRequested, this, &ConnectionTab::emptyDatabase);
    connect(m_browser, &ObjectBrowser::alterDatabaseRequested, this,
            &ConnectionTab::promptAlterDatabase);

    const bool isSqlite = m_params.driverType == SqlDriverType::Sqlite;

    m_browser->setConnectionLabel(isSqlite
                                      ? QFileInfo(m_params.filePath).fileName()
                                      : QStringLiteral("%1@%2").arg(m_params.user, m_params.host));
    m_browser->loadDatabases(m_conn, defaultDb(), m_params.database);
    updateCompletions();
    m_messages->setPlainText(isSqlite
                                 ? QStringLiteral("Connected to %1\nSQLite version: %2")
                                       .arg(m_params.filePath, m_conn->serverInfo())
                                 : QStringLiteral("Connected to %1:%2 as %3\nServer version: %4")
                                       .arg(m_params.host)
                                       .arg(m_params.port)
                                       .arg(m_params.user, m_conn->serverInfo()));

    QStringList dbs;
    if(m_conn)
        dbs = m_conn->listDatabases();
    m_databases = dbs;
    selectSoleSchema();
    emit databasesChanged(dbs, defaultDb());
    updateActiveConnectionLabel();
}

ConnectionTab::~ConnectionTab()
{
    qDeleteAll(m_sideConnections);
    delete m_conn;
}

SqlEditor *ConnectionTab::currentEditor() const
{
    if(auto *ed = qobject_cast<SqlEditor *>(m_editorTabs->currentWidget()))
        return ed;
    /* current tab isn't an editor (e.g. History) — fall back to any editor */
    for(int i = 0; i < m_editorTabs->count(); ++i)
        if(auto *ed = qobject_cast<SqlEditor *>(m_editorTabs->widget(i)))
            return ed;
    return nullptr;
}

void ConnectionTab::attachEditor(SqlEditor *ed, const QString &title)
{
    connect(ed, &QsciScintilla::cursorPositionChanged, this, [this, ed] {
        int line = 0, index = 0;
        ed->getCursorPosition(&line, &index);
        emit cursorMoved(QStringLiteral("Ln %1, Col %2").arg(line + 1).arg(index + 1));
    });
    ed->setDialect(m_params.driverType);
    ed->setCompletionSchema(m_completionSchema);
    Q_UNUSED(title);
}

QString ConnectionTab::driverDisplayLabel() const
{
    if(m_params.driverType == SqlDriverType::Mysql && m_conn &&
       m_conn->serverInfo().contains(QLatin1String("MariaDB"), Qt::CaseInsensitive))
        return QStringLiteral("MariaDB");
    return driverDisplayName(m_params.driverType);
}

QString ConnectionTab::defaultDb() const
{
    if(m_params.driverType == SqlDriverType::Postgres)
        return m_currentSchema.isEmpty() ? QStringLiteral("public") : m_currentSchema;
    /* SQLite names its one database "main" — a file connection carries no
     * database field, so an empty default would make db-less calls (Create
     * Procedure etc.) hit "Select a database first." instead of doing their
     * normal thing (which for PROCEDURE/FUNCTION/EVENT is the SQLite guard) */
    if(m_params.driverType == SqlDriverType::Sqlite)
        return m_params.database.isEmpty() ? QStringLiteral("main") : m_params.database;
    return m_params.database;
}

QString ConnectionTab::activeConnectionLabel() const
{
    if(m_params.driverType == SqlDriverType::Sqlite)
        return m_params.filePath;
    const QString where = QStringLiteral("%1:%2").arg(m_params.host).arg(m_params.port);
    /* the visible grid came from another physical database's side
     * connection — name it, with the marker telling the user why the
     * footer disagrees with the title bar's primary connection */
    if(!m_tableDataPhysDb.isEmpty() && m_resultTabs->currentWidget() == m_tableData)
        return QStringLiteral("%1@%2 — table data").arg(m_tableDataPhysDb, where);
    return QStringLiteral("%1@%2%3").arg(
        m_params.user, where,
        m_params.database.isEmpty() ? QString() : QStringLiteral("/") + m_params.database);
}

void ConnectionTab::updateActiveConnectionLabel()
{
    emit activeConnectionChanged(activeConnectionLabel());
}

ConnectionParams ConnectionTab::paramsFor(const QString &database) const
{
    ConnectionParams p = m_params;
    p.database = database;
    p.name = database;
    return p;
}

IDbConnection *ConnectionTab::connectionFor(const QString &database, QString *error)
{
    if(database.isEmpty() || database == m_params.database)
        return m_conn;
    if(IDbConnection *cached = m_sideConnections.value(database))
        return cached;
    IDbConnection *c = dbDriverFor(m_params.driverType)->connect(paramsFor(database), error);
    if(c) {
        m_sideConnections.insert(database, c);
        /* a connection the user can't see opened — the status bar's
         * "Connections: N" counts live connections, so say it changed
         * (the label itself is unchanged; MainWindow re-counts on every
         * emission) */
        updateActiveConnectionLabel();
    }
    return c;
}

/* upstream ObjectInfo.cpp: Column Information (SHOW FULL FIELDS) + Index
 * Information (SHOW KEYS) + DDL Information (SHOW CREATE) for a table/view;
 * DDL alone for a procedure/function/trigger/event — same three-section
 * shape, built from the portable seam (listColumns/listIndexes/
 * listForeignKeys/showCreate) instead of raw SQL, so it's identical across
 * MySQL/PostgreSQL/SQLite. Foreign Keys is an addition upstream doesn't
 * have as its own section (folded into DDL there); a portable
 * listForeignKeys() already existed for the Foreign Keys dialog, so
 * surfacing it here too costs nothing. */
QString ConnectionTab::buildObjectInfoHtml(IDbConnection *conn, const QString &db,
                                           const QString &objType, const QString &name)
{
    if(!conn)
        return QStringLiteral("<p style='color:#c0392b'>Not connected.</p>");

    /* header row: palette(highlight)/(highlighted-text) rather than a
     * hardcoded blue, so this still looks right in Dark/Twilight (their
     * own highlight colors — see Theme.cpp), matching the reference
     * screenshot's solid-blue header only because Light's highlight
     * happens to be blue. Row striping is inline (alternating background)
     * rather than a ":nth-child" CSS rule — QTextDocument's HTML/CSS
     * support is a limited subset and doesn't reliably cover that
     * selector, so this is the one way guaranteed to render everywhere. */
    const auto sectionHeader = [](const QString &title, int count = -1) {
        return QStringLiteral("<h4>%1%2</h4>")
            .arg(title.toHtmlEscaped(), count < 0 ? QString() : QStringLiteral(" (%1)").arg(count));
    };
    const auto tableOpen = [](std::initializer_list<const char *> cols) {
        QString h = QStringLiteral("<table><tr>");
        for(const char *c : cols)
            h += QStringLiteral("<th>%1</th>").arg(QLatin1String(c));
        return h + QStringLiteral("</tr>");
    };
    const auto rowOpen = [](int i) {
        return i % 2 ? QStringLiteral("<tr style='background:palette(alternate-base)'>")
                     : QStringLiteral("<tr>");
    };

    const QString nice = objType.left(1) + objType.mid(1).toLower();
    QString html = QStringLiteral(
        "<style>"
        "table{border-collapse:collapse;margin:4px 0 14px 0}"
        "th,td{border:1px solid palette(mid);padding:3px 8px;font-size:12px;text-align:left}"
        "th{background:palette(highlight);color:palette(highlighted-text);font-weight:bold}"
        "h3{border-bottom:2px solid palette(highlight);padding-bottom:4px;margin-bottom:10px}"
        "h4{margin:14px 0 4px 0}"
        "pre{background:palette(alternate-base);border:1px solid palette(mid);"
        "padding:6px;white-space:pre-wrap;font-size:12px}"
        "</style>");
    html += QStringLiteral("<h3>%1: %2</h3>").arg(nice, name.toHtmlEscaped());

    if(objType == QStringLiteral("TABLE") || objType == QStringLiteral("VIEW")) {
        const DbResultSet cols = conn->listColumns(db, name);
        html += sectionHeader(QStringLiteral("Columns"), cols.rows.size());
        html += tableOpen({"Field", "Type", "Null", "Key", "Default", "Extra", "Comment"});
        int i = 0;
        for(const QStringList &row : cols.rows) {
            html += rowOpen(i++);
            /* 🔑 next to a primary-key field — Unicode text, no image asset
             * needed, renders the same in every theme */
            const bool pk = row.value(3) == QStringLiteral("PRI");
            html += QStringLiteral("<td>%1%2</td>")
                        .arg(pk ? QStringLiteral("🔑 ") : QString(), row.value(0).toHtmlEscaped());
            for(int c = 1; c < 7; ++c)
                html += QStringLiteral("<td>%1</td>").arg(row.value(c).toHtmlEscaped());
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");

        if(objType == QStringLiteral("TABLE")) {
            const DbResultSet idx = conn->listIndexes(db, name);
            html += sectionHeader(QStringLiteral("Index Information"), idx.rows.size());
            if(idx.rows.isEmpty()) {
                html += QStringLiteral("<p style='color:palette(disabled-text)'>No indexes.</p>");
            } else {
                html += tableOpen({"Key name", "Column", "Seq", "Unique"});
                i = 0;
                for(const QStringList &row : idx.rows)
                    html += rowOpen(i++) +
                            QStringLiteral("<td>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>")
                                .arg(row.value(2).toHtmlEscaped(), row.value(4).toHtmlEscaped(),
                                     row.value(3).toHtmlEscaped(),
                                     row.value(1) == QStringLiteral("0") ? QStringLiteral("yes")
                                                                         : QStringLiteral("no"));
                html += QStringLiteral("</table>");
            }

            const DbResultSet fks = conn->listForeignKeys(db, name);
            if(!fks.rows.isEmpty()) {
                html += sectionHeader(QStringLiteral("Foreign Keys"), fks.rows.size());
                html += tableOpen({"Name", "Column", "References", "On Update", "On Delete"});
                i = 0;
                for(const QStringList &row : fks.rows)
                    html += rowOpen(i++) +
                            QStringLiteral("<td>%1</td><td>%2</td><td>%3.%4</td>"
                                           "<td>%5</td><td>%6</td></tr>")
                                .arg(row.value(0).toHtmlEscaped(), row.value(1).toHtmlEscaped(),
                                     row.value(2).toHtmlEscaped(), row.value(3).toHtmlEscaped(),
                                     row.value(4).toHtmlEscaped(), row.value(5).toHtmlEscaped());
                html += QStringLiteral("</table>");
            }
        }
    }

    html += sectionHeader(QStringLiteral("DDL Information"));
    QString error;
    const QString ddl = conn->showCreate(objType, db, name, &error);
    html += (ddl.isEmpty() && !error.isEmpty())
                ? QStringLiteral("<p style='color:#c0392b'>%1</p>").arg(error.toHtmlEscaped())
                : QStringLiteral("<pre>%1</pre>").arg(ddl.toHtmlEscaped());
    return html;
}

void ConnectionTab::updateInfoTab(const QString &db, const QString &objType, const QString &name,
                                  const QString &physDb)
{
    QString error;
    IDbConnection *conn = connectionFor(physDb, &error);
    m_info->setHtml(
        conn ? buildObjectInfoHtml(conn, db, objType, name)
             : QStringLiteral("<p style='color:#c0392b'>%1</p>").arg(error.toHtmlEscaped()));
}

bool ConnectionTab::switchDatabase(const QString &database)
{
    if(!m_conn || database.isEmpty() || database == m_params.database)
        return false;
    /* connectionFor() below opens a brand-new network connection when
     * `database` isn't already cached — a real, blocking round trip on
     * the GUI thread, with nothing to show for it otherwise (this is
     * PostgreSQL-only in practice: the Object Browser's tree is the only
     * caller, and it only ever emits switchDatabaseRequested for a
     * Postgres database node — MySQL/SQLite have no such node at all,
     * useDatabase()'s SET/USE path handles every one of their picks) */
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_messages->setPlainText(QStringLiteral("Connecting to %1…").arg(database));
    m_resultTabs->setCurrentWidget(m_messages);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    QString error;
    IDbConnection *newConn = connectionFor(database, &error);
    if(!newConn) {
        QApplication::restoreOverrideCursor();
        m_messages->setPlainText(QStringLiteral("Could not switch to %1: %2").arg(database, error));
        m_resultTabs->setCurrentWidget(m_messages);
        return false;
    }
    /* newConn just came from the side-connection pool (freshly opened and
     * cached there, or already cached from earlier browsing) — pull it out
     * since it's becoming primary, and put the outgoing primary in under
     * its own name instead, so switching back later is instant rather
     * than reconnecting */
    m_sideConnections.remove(database);
    m_sideConnections.insert(m_params.database, m_conn);
    m_conn = newConn;
    m_params.database = database;
    m_params.name = database;
    m_currentSchema.clear(); /* belonged to the old database's search_path */
    /* whatever rows the Table Data grid still holds were loaded through a
     * connection that just moved into (or out of) the side pool — no
     * longer a valid "the active connection is X" answer; the switch's
     * setCurrentWidget(Messages) below re-emits with the new primary */
    m_tableDataPhysDb.clear();

    /* false: this switch was triggered from an already-browsed tree (a
     * double-click on a database node, or the toolbar combo) — just show
     * the newly-primary database's schema list, matching what expanding
     * its arrow would show, rather than also auto-diving into its Tables
     * folder like a fresh connection does */
    refreshBrowser(false);
    m_messages->setPlainText(QStringLiteral("Switched to %1").arg(database));
    m_resultTabs->setCurrentWidget(m_messages);

    const QStringList dbs = m_conn->listDatabases();
    m_databases = dbs;
    selectSoleSchema();
    emit databasesChanged(dbs, defaultDb());
    QApplication::restoreOverrideCursor();
    return true;
}

/* what the editors' autocomplete knows about the current database (see
 * SqlCompletion.h): its tables, views, columns and routines up front; one
 * table's columns, or another database's tables, looked up the first time a
 * statement needs them and cached until the next refresh */
void ConnectionTab::updateCompletions()
{
    m_columnCache.clear();
    m_tablesCache.clear();
    SqlCompletion::Schema s;
    const QString db = defaultDb();
    if(m_conn && !db.isEmpty()) {
        s.database = db;
        s.tables = m_conn->listTables(db, QStringLiteral("BASE TABLE"));
        s.views = m_conn->listTables(db, QStringLiteral("VIEW"));
        s.tables.removeDuplicates();
        s.views.removeDuplicates();
        /* one bulk query instead of a listColumns() round trip per table —
         * see IDbConnection::listAllColumnNames() */
        s.columns = m_conn->listAllColumnNames(db);
        /* one row per column pair, a composite key's rows consecutive */
        QString lastKey;
        for(const QStringList &row : m_conn->listAllForeignKeys(db).rows) {
            const QString key = row.value(0) + QLatin1Char('\x1f') + row.value(1);
            if(key != lastKey)
                s.foreignKeys.append({row.value(0), {}, row.value(3), {}});
            lastKey = key;
            s.foreignKeys.last().columns << row.value(2);
            s.foreignKeys.last().refColumns << row.value(4);
        }
        for(const QStringList &row : m_conn->listRoutines(db).rows) {
            const bool proc =
                row.value(1).compare(QLatin1String("PROCEDURE"), Qt::CaseInsensitive) == 0;
            (proc ? s.procedures : s.functions) << row.value(0);
        }
    }
    if(m_conn)
        s.databases = m_conn->listDatabases();
    s.columnsOf = [this](const QString &table) {
        if(!m_conn)
            return QStringList();
        const QString db = defaultDb();
        const QString key = db + QLatin1Char('\x1f') + table;
        auto it = m_columnCache.find(key);
        if(it == m_columnCache.end()) {
            QStringList cols;
            for(const QStringList &row : m_conn->listColumns(db, table).rows)
                cols << row.value(0);
            it = m_columnCache.insert(key, cols);
        }
        return it.value();
    };
    s.tablesOf = [this](const QString &other) {
        if(!m_conn)
            return QStringList();
        auto it = m_tablesCache.find(other);
        if(it == m_tablesCache.end())
            it = m_tablesCache.insert(other, m_conn->listTables(other));
        return it.value();
    };
    m_completionSchema = s;
    for(int i = 0; i < m_editorTabs->count(); ++i)
        if(auto *ed = qobject_cast<SqlEditor *>(m_editorTabs->widget(i)))
            ed->setCompletionSchema(m_completionSchema);
}

void ConnectionTab::addEditorTab()
{
    int maxN = 0;
    for(int i = 0; i < m_editorTabs->count(); ++i) {
        const QString t = m_editorTabs->tabText(i);
        if(t.startsWith(QStringLiteral("Query ")))
            maxN = qMax(maxN, t.mid(6).toInt());
    }
    openEditorWithSql(QStringLiteral("Query %1").arg(maxN + 1), QString());
}

void ConnectionTab::closeEditorTab(int index)
{
    QWidget *w = m_editorTabs->widget(index);
    if(!w)
        return;

    /* History is a member page — detach it, don't delete; reopen via the
     * History button / menu */
    if(w == m_historyPage) {
        m_editorTabs->removeTab(index);
        return;
    }

    m_editorTabs->removeTab(index);
    if(w == m_editor)
        m_editor = nullptr;
    w->deleteLater();

    /* keep at least one query editor open */
    bool haveEditor = false;
    for(int i = 0; i < m_editorTabs->count(); ++i)
        if(qobject_cast<SqlEditor *>(m_editorTabs->widget(i)))
            haveEditor = true;
    if(!haveEditor)
        addEditorTab();
    if(!m_editor)
        m_editor = currentEditor();
}

void ConnectionTab::closeResultTab(int index)
{
    QWidget *w = m_resultTabs->widget(index);
    /* Messages/Table Data/Info never show a close button (see the
     * setTabButton() calls in the constructor), so this should only ever
     * be reached for a dynamic "Execute Query N" tab — the guard is
     * defensive, not load-bearing */
    if(!w || w == m_messages || w == m_tableData || w == m_info)
        return;
    m_resultTabs->removeTab(index);
    m_dynamicResultTabs.removeOne(w);
    if(w == m_lastGrid)
        m_lastGrid = nullptr;
    w->deleteLater();
}

void ConnectionTab::editorCopyNormalizedWhitespace()
{
    if(SqlEditor *ed = currentEditor())
        ed->copyWithNormalizedWhitespace();
}

void ConnectionTab::editorInsertFromFile()
{
    if(SqlEditor *ed = currentEditor())
        ed->insertFromFile();
}

void ConnectionTab::collapseBrowser()
{
    if(m_browser)
        m_browser->collapseTree();
}

void ConnectionTab::renameCurrentEditorTab()
{
    const int idx = m_editorTabs->currentIndex();
    if(idx < 0 || m_editorTabs->widget(idx) == m_historyPage)
        return;
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("Rename Tab"), QStringLiteral("Tab name:"),
                              QLineEdit::Normal, m_editorTabs->tabText(idx), &ok);
    if(ok && !name.trimmed().isEmpty())
        m_editorTabs->setTabText(idx, name.trimmed());
}

void ConnectionTab::dumpTable(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    /* same dialog as Backup Database, the table pre-checked — upstream's
     * Backup Table(s) is the same wizard over a one-table selection */
    runDumpDialog(db, {table});
}

SqlEditor *ConnectionTab::openEditorWithSql(const QString &title, const QString &sql)
{
    auto *ed = new SqlEditor(this);
    attachEditor(ed, {});
    if(!sql.isEmpty())
        ed->setPlainText(sql);
    const int histIdx = m_editorTabs->indexOf(m_historyPage);
    m_editorTabs->insertTab(histIdx == -1 ? m_editorTabs->count() : histIdx, ed,
                            Icons::get(QStringLiteral("query_16.ico")), title);
    const int idx = m_editorTabs->indexOf(ed);
    m_editorTabs->setTabToolTip(idx, title);
    m_editorTabs->setCurrentWidget(ed);
    return ed;
}

void ConnectionTab::logHistory(const QString &sql)
{
    QString flat = sql;
    flat.replace('\n', QLatin1Char(' ')).replace('\r', QString());
    m_historyLines << QStringLiteral("[%1] %2").arg(
        QTime::currentTime().toString(QStringLiteral("hh:mm:ss")), flat);
    m_historyQueries << sql; /* keep the original line breaks */
    renderHistory();
    appendHistoryLine(m_params.name, sql);
}

void ConnectionTab::sendHistoryToEditor(const QString &sql)
{
    auto *ed = currentEditor();
    if(!ed || sql.isEmpty())
        return;
    QString cur = ed->toPlainText();
    if(!cur.isEmpty() && !cur.endsWith('\n'))
        cur += QLatin1Char('\n');
    ed->setPlainText(cur + sql + (sql.trimmed().endsWith(';') ? QString() : QStringLiteral(";")) +
                     QLatin1Char('\n'));
    ed->moveCursorToEnd();
    m_editorTabs->setCurrentWidget(ed);
    ed->setFocus();
}

void ConnectionTab::renderHistory()
{
    const QString filter = m_historySearch->text().trimmed();
    QString html = QStringLiteral("<style>a{text-decoration:none;font-size:13px}"
                                  ".ts{color:#8a8a8a}"
                                  ".q{white-space:pre-wrap}</style>"
                                  "<table cellspacing='0' cellpadding='1'>");
    for(int i = 0; i < m_historyLines.size(); ++i) {
        const QString &l = m_historyLines[i];
        if(!filter.isEmpty() && !l.contains(filter, Qt::CaseInsensitive))
            continue;
        if(m_historyQueries.value(i).isEmpty()) { /* session divider */
            html += QStringLiteral("<tr><td></td><td><i>%1</i></td></tr>")
                        .arg(l.trimmed().toHtmlEscaped());
            continue;
        }
        const int rb = l.indexOf(']');
        const QString tsPart = rb > 0 ? l.left(rb + 1) : QString();
        html += QStringLiteral("<tr><td valign='top' style='white-space:nowrap'>"
                               "<a href='c:%1' title='Copy query'>⧉</a>&#160;"
                               "<a href='e:%1' title='Send to editor (formatted)'>&#8618;</a>"
                               "&#160;&#160;</td>"
                               "<td><span class='ts'>%2</span> <span class='q'>%3</span></td></tr>")
                    .arg(i)
                    .arg(tsPart.toHtmlEscaped(), m_historyQueries[i].toHtmlEscaped());
    }
    html += QStringLiteral("</table>");
    m_history->setHtml(html);
    m_history->verticalScrollBar()->setValue(m_history->verticalScrollBar()->maximum());
}

void ConnectionTab::copyAllShownHistory()
{
    const QString filter = m_historySearch->text().trimmed();
    QStringList out;
    for(int i = 0; i < m_historyLines.size(); ++i) {
        if(m_historyQueries.value(i).isEmpty())
            continue;
        if(!filter.isEmpty() && !m_historyLines[i].contains(filter, Qt::CaseInsensitive))
            continue;
        QString q = m_historyQueries[i].trimmed();
        if(!q.endsWith(';'))
            q += QLatin1Char(';');
        out << q;
    }
    if(out.isEmpty())
        return;
    QApplication::clipboard()->setText(out.join(QStringLiteral("\n\n")));
    emit executed(QStringLiteral("Copied %1 quer%2 to the clipboard")
                      .arg(out.size())
                      .arg(out.size() == 1 ? "y" : "ies"));
}

void ConnectionTab::clearHistory()
{
    if(QMessageBox::question(
           this, QStringLiteral("Clear History"),
           QStringLiteral("Delete all saved query history for every connection?")) !=
       QMessageBox::Yes)
        return;
    QFile(historyPath()).resize(0);
    m_historyLines.clear();
    m_historyQueries.clear();
    renderHistory();
}

void ConnectionTab::addCurrentToFavorites()
{
    SqlEditor *ed = currentEditor();
    if(!ed)
        return;
    QString sql = ed->hasSelectedText() ? ed->selectedText() : ed->toPlainText();
    sql = sql.trimmed();
    if(sql.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Add To Favorites"),
                                 QStringLiteral("Nothing to save — the editor is empty."));
        return;
    }
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("Add To Favorites"), QStringLiteral("Name:"),
                              QLineEdit::Normal, QString(), &ok);
    if(!ok || name.trimmed().isEmpty())
        return;
    FavoritesStore::save(name.trimmed(), sql);
    emit executed(QStringLiteral("Saved favorite \"%1\"").arg(name.trimmed()));
}

void ConnectionTab::insertFavorite(const QString &name)
{
    sendHistoryToEditor(FavoritesStore::get(name));
}

void ConnectionTab::organizeFavorites()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Organize Favorites"));
    dlg.resize(420, 320);
    auto *list = new QListWidget(&dlg);
    list->addItems(FavoritesStore::names());
    auto *insertBtn = new QPushButton(QStringLiteral("&Insert Into Editor"), &dlg);
    auto *renameBtn = new QPushButton(QStringLiteral("&Rename…"), &dlg);
    auto *deleteBtn = new QPushButton(QStringLiteral("&Delete"), &dlg);
    for(QPushButton *b : {insertBtn, renameBtn, deleteBtn})
        b->setEnabled(false);
    connect(list, &QListWidget::currentRowChanged, &dlg, [=](int row) {
        for(QPushButton *b : {insertBtn, renameBtn, deleteBtn})
            b->setEnabled(row >= 0);
    });
    connect(insertBtn, &QPushButton::clicked, &dlg, [=, this, &dlg] {
        if(auto *item = list->currentItem()) {
            insertFavorite(item->text());
            dlg.accept();
        }
    });
    connect(list, &QListWidget::itemDoubleClicked, &dlg, [=, this, &dlg](QListWidgetItem *item) {
        insertFavorite(item->text());
        dlg.accept();
    });
    connect(renameBtn, &QPushButton::clicked, &dlg, [=, &dlg] {
        auto *item = list->currentItem();
        if(!item)
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(&dlg, QStringLiteral("Rename Favorite"),
                                                   QStringLiteral("New name:"), QLineEdit::Normal,
                                                   item->text(), &ok);
        if(ok && !name.trimmed().isEmpty() && FavoritesStore::rename(item->text(), name.trimmed()))
            item->setText(name.trimmed());
    });
    connect(deleteBtn, &QPushButton::clicked, &dlg, [=, &dlg] {
        auto *item = list->currentItem();
        if(!item)
            return;
        if(QMessageBox::question(&dlg, QStringLiteral("Delete Favorite"),
                                 QStringLiteral("Delete \"%1\"?").arg(item->text())) !=
           QMessageBox::Yes)
            return;
        FavoritesStore::remove(item->text());
        delete item;
    });
    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(insertBtn);
    btnRow->addWidget(renameBtn);
    btnRow->addWidget(deleteBtn);
    btnRow->addStretch(1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(list, 1);
    lay->addLayout(btnRow);
    lay->addWidget(buttons);
    dlg.exec();
}

/* F9 — run the selected text, or the statement under the cursor */
void ConnectionTab::runQuery()
{
    SqlEditor *ed = currentEditor();
    if(!ed)
        return;
    const QString sql = ed->hasSelectedText() ? ed->selectedText()
                                              : statementAt(ed->toPlainText(), ed->cursorPosition(),
                                                            m_params.driverType);
    runStatements(splitStatements(sql, m_params.driverType), QStringLiteral("Execute Query"));
}

/* Ctrl+F9 — run the whole editor */
void ConnectionTab::runAll()
{
    if(SqlEditor *ed = currentEditor())
        runStatements(splitStatements(ed->toPlainText(), m_params.driverType),
                      QStringLiteral("Execute Query"));
}

/* F8 — run the current statement, and if it's a single-table SELECT open that
 * table in the editable Table Data pane */
void ConnectionTab::runAndEdit()
{
    SqlEditor *ed = currentEditor();
    if(!ed)
        return;
    const QString stmt =
        (ed->hasSelectedText()
             ? ed->selectedText()
             : statementAt(ed->toPlainText(), ed->cursorPosition(), m_params.driverType))
            .trimmed();

    static const QRegularExpression re(QStringLiteral("^SELECT\\b.*\\bFROM\\s+`?([A-Za-z0-9_$]+)`?"
                                                      "(?:\\.`?([A-Za-z0-9_$]+)`?)?"),
                                       QRegularExpression::CaseInsensitiveOption |
                                           QRegularExpression::DotMatchesEverythingOption);
    const auto m = re.match(stmt);
    /* reject if a second table is joined/comma'd */
    const bool multiTable = stmt.contains(QRegularExpression(
        QStringLiteral("\\bJOIN\\b|\\bFROM\\b[^,]+,"), QRegularExpression::CaseInsensitiveOption));

    if(m.hasMatch() && !multiTable) {
        const QString db = m.captured(2).isEmpty() ? defaultDb() : m.captured(1);
        const QString table = m.captured(2).isEmpty() ? m.captured(1) : m.captured(2);
        openTableData(db, table);
        emit executed(QStringLiteral("Editing `%1`.`%2` in Table Data").arg(db, table));
        return;
    }
    m_messages->appendPlainText(
        QStringLiteral("F8 needs a single-table SELECT to edit — running read-only."));
    runQuery();
}

/* Explain the current statement (FORMAT=JSON goes to the Messages pane).
 * Plain EXPLAIN is valid SQL on all three drivers as-is (SQLite's own
 * EXPLAIN just returns raw VDBE opcodes rather than a query plan — usable,
 * just not very readable — so it's left unguarded); FORMAT=JSON is where
 * the syntax actually diverges: MySQL's own "EXPLAIN FORMAT=JSON <stmt>"
 * is invalid everywhere else — Postgres needs the parenthesized option
 * list "EXPLAIN (FORMAT JSON) <stmt>", and SQLite has no such option at
 * all, so that combination is guarded instead of sent as broken SQL. */
void ConnectionTab::explainCurrent(bool json)
{
    SqlEditor *ed = currentEditor();
    if(!ed)
        return;
    QString stmt = ed->hasSelectedText()
                       ? ed->selectedText()
                       : statementAt(ed->toPlainText(), ed->cursorPosition(), m_params.driverType);
    stmt = stmt.trimmed();
    while(stmt.endsWith(QLatin1Char(';')))
        stmt.chop(1);
    if(stmt.isEmpty())
        return;
    QString explainSql;
    if(!json) {
        explainSql = QStringLiteral("EXPLAIN %1").arg(stmt);
    } else if(driverType() == SqlDriverType::Postgres) {
        explainSql = QStringLiteral("EXPLAIN (FORMAT JSON) %1").arg(stmt);
    } else if(driverType() == SqlDriverType::Sqlite) {
        QMessageBox::information(this, QStringLiteral("Explain"),
                                 QStringLiteral("SQLite has no EXPLAIN FORMAT=JSON — use plain "
                                                "EXPLAIN instead."));
        return;
    } else {
        explainSql = QStringLiteral("EXPLAIN FORMAT=JSON %1").arg(stmt);
    }
    runStatements({explainSql}, QStringLiteral("Explain"));
}

/* headless-test hook (--explain=json|plain): put a known statement in the
 * editor and run explainCurrent() on it — exactly the path the Explain /
 * Explain Format=JSON menu items take. Goes through runStatements(), so the
 * result (plan or server error) lands in the result area for the
 * --screenshot= pass to capture. Threaded, like every real run. */
void ConnectionTab::selftestExplain(const QString &mode)
{
    SqlEditor *ed = currentEditor();
    if(!ed)
        return;
    ed->setPlainText(QStringLiteral("SELECT 1"));
    ed->moveCursorToEnd();
    explainCurrent(mode == QLatin1String("json"));
}

QString ConnectionTab::selftestShowDataTab()
{
    m_resultTabs->setCurrentWidget(m_tableData);
    return m_tableData->loadedTable();
}

QString ConnectionTab::selftestInfoSearch(const QString &text)
{
    m_info->setSearchForTest(text);
    return QStringLiteral("%1 matches; label '%2'")
        .arg(m_info->matchCountForTest())
        .arg(m_info->countTextForTest());
}

void ConnectionTab::selftestShowInfoTab()
{
    m_resultTabs->setCurrentWidget(m_info);
}

void ConnectionTab::runStatements(const QStringList &statements, const QString &tabPrefix)
{
    if(m_running) {
        m_messages->appendPlainText(QStringLiteral("a batch is already running…"));
        return;
    }
    if(statements.isEmpty())
        return;

    for(const QString &s : statements)
        logHistory(s);

    m_running = true;
    const int gen = ++m_batchGen;
    m_messages->setPlainText(QStringLiteral("Executing %1 statement(s)…").arg(statements.size()));
    m_resultTabs->setCurrentWidget(m_messages);

    /* worker thread: fresh connection, plain-data results. Holds its own
     * shared_ptr to the cancel-state, so closing this tab mid-query (guard
     * turning null) can't leave the thread holding a dangling pointer.
     *
     * Runs on the global QThreadPool rather than a detached std::thread —
     * a detached thread can't be waited on by anything, which was a real,
     * live bug (session 79/83): every exit path in main.cpp calls the MySQL/
     * Postgres/SQLite driver's libraryShutdown() right after QApplication::
     * exec() returns, and a detached query thread still inside a blocking
     * mysql_real_query()/PQexec() call at that moment races the client
     * library's own global teardown — the intermittent ~40% crash
     * --dumpcombo hit and worked around with a delay-then-quit shape rather
     * than fixing. QThreadPool::globalInstance()->waitForDone(), called
     * once right before each libraryShutdown() site, blocks until every
     * such task has genuinely returned — the fix belongs there, not here;
     * this is just what makes that possible to wait for at all. */
    QPointer<ConnectionTab> guard(this);
    const ConnectionParams p = m_params;
    std::shared_ptr<LiveConnection> cancelState = m_cancelState;
    QThreadPool::globalInstance()->start([guard, p, statements, tabPrefix, cancelState] {
        const QVector<QueryResult> results = runOnConnection(p, statements, cancelState.get());
        QMetaObject::invokeMethod(
            guard,
            [guard, results, tabPrefix] {
                if(guard)
                    guard->applyResults(results, tabPrefix);
            },
            Qt::QueuedConnection);
    });

    /* the timeout re-checks m_batchGen before cancelling: without it, a
     * timer armed for a slow batch that finishes early (or is cancelled by
     * hand) would fire late and cancel whatever *next* batch happens to be
     * running at that moment. */
    const int timeoutSecs = loadQueryTimeoutSecs();
    if(timeoutSecs > 0) {
        QTimer::singleShot(timeoutSecs * 1000, guard, [guard, gen] {
            if(guard && guard->m_running && guard->m_batchGen == gen)
                guard->cancelQuery();
        });
    }
}

int ConnectionTab::queryTimeoutSecs()
{
    return loadQueryTimeoutSecs();
}
void ConnectionTab::setQueryTimeoutSecs(int secs)
{
    saveQueryTimeoutSecs(secs);
}

void ConnectionTab::cancelQuery()
{
    QMutexLocker lock(&m_cancelState->mutex);
    if(m_cancelState->live)
        m_cancelState->live->cancel();
    else
        m_messages->appendPlainText(QStringLiteral("nothing is running"));
}

void ConnectionTab::openTable(const QString &db, const QString &table)
{
    if(m_running || !m_conn)
        return;
    const QString sql =
        QStringLiteral("SELECT * FROM %1 LIMIT 1000").arg(m_conn->qualify(db, table));
    logHistory(sql);
    runStatements(QStringList{sql}, QStringLiteral("%1.%2").arg(db, table));
}

void ConnectionTab::applyResults(const QVector<QueryResult> &results, const QString &tabPrefix)
{
    m_running = false;

    QString summary;
    int grids = 0;
    double total = 0.0;
    QWidget *firstGrid = nullptr;

    for(int i = 0; i < results.size(); ++i) {
        const QueryResult &r = results[i];
        total += r.secs;
        m_totalSecs += r.secs;

        if(!r.ok) {
            summary += QStringLiteral("✗ [%1] %2 (%3 sec)\n")
                           .arg(i + 1)
                           .arg(r.message)
                           .arg(r.secs, 0, 'f', 2);
            continue;
        }
        summary +=
            QStringLiteral("✓ [%1] %2 (%3 sec)\n").arg(i + 1).arg(r.message).arg(r.secs, 0, 'f', 2);

        if(!r.headers.isEmpty()) {
            ++grids;
            /* ever-increasing counter, not "grids" (which restarts at 1
             * every call) — old result tabs are never cleared anymore, so
             * a per-call counter would produce duplicate titles ("Execute
             * Query 1" appearing again on the next run) */
            addResultGrid(r, QStringLiteral("%1 %2").arg(tabPrefix).arg(++m_resultTabCounter));
            if(!firstGrid)
                firstGrid = m_dynamicResultTabs.last();
        }
    }

    if(grids)
        summary += QStringLiteral("\n%1 result set(s) displayed.").arg(grids);
    m_messages->setPlainText(summary);
    m_resultTabs->setCurrentWidget(firstGrid ? firstGrid : static_cast<QWidget *>(m_messages));

    const double exec = total;
    emit executed(QStringLiteral("Exec: %1 sec | Total: %2 sec")
                      .arg(exec, 0, 'f', 2)
                      .arg(m_totalSecs, 0, 'f', 2));
}

void ConnectionTab::addResultGrid(const QueryResult &r, const QString &title)
{
    auto *model = new QueryModel(this);
    model->setResultSet(r.headers, r.rows);
    auto *proxy = new GridSortProxy(this);
    proxy->setSourceModel(model);

    auto *grid = new QTableView(this);
    grid->setModel(proxy);
    /* nothing sorted until the user clicks a header: setSortingEnabled()
     * sorts by the header's indicator right away, and its default (column 0,
     * descending) silently reversed the query's own ORDER BY */
    grid->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    grid->setSortingEnabled(true);
    grid->setAlternatingRowColors(true);
    grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    grid->setSelectionBehavior(QAbstractItemView::SelectItems);
    grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid->horizontalHeader()->setStretchLastSection(true);
    grid->horizontalHeader()->setSectionsMovable(true);
    grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    installGridCopy(grid);
    wireResultGrid(grid);

    m_resultTabs->addTab(grid, Icons::get(QStringLiteral("grid_view.ico")), title);
    m_dynamicResultTabs.append(grid);
    m_lastGrid = grid;
}

/* SQLyog-style right-click menu on a query-result grid: view a cell, copy
 * rows as TSV / INSERT, copy a column, export the selection */
void ConnectionTab::wireResultGrid(QTableView *grid)
{
    grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid, &QTableView::customContextMenuRequested, grid, [this, grid](const QPoint &pos) {
        QAbstractItemModel *m = grid->model();
        if(!m || m->rowCount() == 0)
            return;
        const QModelIndex at = grid->indexAt(pos);
        const QModelIndexList sel =
            grid->selectionModel() ? grid->selectionModel()->selectedIndexes() : QModelIndexList();

        /* unique sorted row / column sets from the selection (or the clicked
         * cell if nothing is selected) */
        QList<int> rowSet, colSet;
        for(const QModelIndex &i : sel) {
            if(!rowSet.contains(i.row()))
                rowSet << i.row();
            if(!colSet.contains(i.column()))
                colSet << i.column();
        }
        if(rowSet.isEmpty() && at.isValid()) {
            rowSet << at.row();
            colSet << at.column();
        }
        std::sort(rowSet.begin(), rowSet.end());
        std::sort(colSet.begin(), colSet.end());
        if(rowSet.isEmpty())
            return;

        const int cols = m->columnCount();
        QStringList headers;
        for(int c = 0; c < cols; ++c)
            headers << m->headerData(c, Qt::Horizontal).toString();

        QMenu menu(grid);
        if(at.isValid())
            menu.addAction(QStringLiteral("&View Cell…"), grid, [grid, at] {
                QDialog d(grid);
                d.setWindowTitle(
                    QStringLiteral("Cell — %1")
                        .arg(grid->model()->headerData(at.column(), Qt::Horizontal).toString()));
                d.resize(520, 320);
                auto *tv = new QPlainTextEdit(&d);
                tv->setReadOnly(true);
                tv->setPlainText(at.data().toString());
                tv->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
                auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
                connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
                connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
                auto *l = new QVBoxLayout(&d);
                l->addWidget(tv, 1);
                l->addWidget(bb);
                d.exec();
            });
        menu.addSeparator();

        menu.addAction(QStringLiteral("Copy Row(s) as &TSV"), grid, [=] {
            ResultExport::Options o;
            o.header = true;
            const std::function<QString(int, int)> cell = [=](int r, int c) {
                return m->index(rowSet.at(r), c).data().toString();
            };
            QApplication::clipboard()->setText(ResultExport::render(
                ResultExport::Format::Tsv, headers, cell, rowSet.size(), cols, o));
        });
        menu.addAction(QStringLiteral("Copy Row(s) as &INSERT"), grid, [=] {
            ResultExport::Options o;
            o.sqlTable = QStringLiteral("result");
            o.driver = m_params.driverType;
            const std::function<QString(int, int)> cell = [=](int r, int c) {
                return m->index(rowSet.at(r), c).data().toString();
            };
            QApplication::clipboard()->setText(ResultExport::render(
                ResultExport::Format::Sql, headers, cell, rowSet.size(), cols, o));
        });
        if(!colSet.isEmpty())
            menu.addAction(QStringLiteral("Copy &Column"), grid, [=] {
                QStringList vals;
                for(int r : rowSet)
                    for(int c : colSet)
                        vals << m->index(r, c).data().toString();
                QApplication::clipboard()->setText(vals.join(QLatin1Char('\n')));
            });
        menu.addSeparator();
        menu.addAction(QStringLiteral("&Export Selection / Result…"), this,
                       &ConnectionTab::exportResult);
        menu.exec(grid->viewport()->mapToGlobal(pos));
    });
}

void ConnectionTab::openSqlFile(const QString &path)
{
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    if(auto *ed = currentEditor())
        ed->setPlainText(QString::fromUtf8(f.readAll()));
    m_editorTabs->setCurrentIndex(0);
    RecentFiles::push(path); /* every open path feeds File > Recent Files */
}

void ConnectionTab::saveEditor()
{
    const QString f =
        QFileDialog::getSaveFileName(this, QStringLiteral("Save SQL"), QStringLiteral("query.sql"),
                                     QStringLiteral("SQL (*.sql);;All (*)"));
    if(f.isEmpty())
        return;
    QFile file(f);
    if(auto *ed = currentEditor())
        if(file.open(QIODevice::WriteOnly | QIODevice::Text))
            file.write(ed->toPlainText().toUtf8());
    RecentFiles::push(f); /* upstream records saves too (WriteLatestFile) */
}

void ConnectionTab::showHistory()
{
    if(m_editorTabs->indexOf(m_historyPage) == -1) /* was closed — bring it back */
        m_editorTabs->addTab(m_historyPage, Icons::get(QStringLiteral("history.ico")),
                             QStringLiteral("History"));
    m_editorTabs->setCurrentWidget(m_historyPage);
}

void ConnectionTab::exportResult()
{
    QAbstractItemModel *m = m_lastGrid ? m_lastGrid->model() : nullptr;
    if(!m || m->rowCount() == 0) {
        m_messages->appendPlainText(QStringLiteral("no result set to export"));
        return;
    }
    const int cols = m->columnCount();
    QStringList headers;
    for(int c = 0; c < cols; ++c)
        headers << m->headerData(c, Qt::Horizontal).toString();

    /* checked/selected rows, if any */
    QList<int> sel;
    if(m_lastGrid->selectionModel())
        for(const QModelIndex &i : m_lastGrid->selectionModel()->selectedRows())
            sel << i.row();
    std::sort(sel.begin(), sel.end());

    ExportDialog dlg(QStringLiteral("result"), QStringLiteral("exported"), m->rowCount(),
                     !sel.isEmpty(), this);
    if(dlg.exec() != QDialog::Accepted || dlg.path().isEmpty())
        return;

    const bool selOnly = dlg.selectionOnly() && !sel.isEmpty();
    const int rows = selOnly ? sel.size() : m->rowCount();
    const auto cell = [&](int r, int c) {
        const int rr = selOnly ? sel.at(r) : r;
        return m->index(rr, c).data().toString();
    };
    ResultExport::Options opt = dlg.options();
    opt.driver = m_params.driverType;
    QString err;
    if(ResultExport::write(dlg.path(), dlg.format(), headers, cell, rows, cols, opt, &err))
        m_messages->appendPlainText(
            QStringLiteral("Exported %1 row(s) → %2").arg(rows).arg(dlg.path()));
    else
        m_messages->appendPlainText(QStringLiteral("Export failed: ") + err);
    m_resultTabs->setCurrentWidget(m_messages);
}

void ConnectionTab::refreshBrowser(bool autoDrill)
{
    if(m_conn) {
        m_browser->loadDatabases(m_conn, defaultDb(), m_params.database, autoDrill);
        updateCompletions();
    }
}

void ConnectionTab::expandDatabaseNode(const QString &name)
{
    m_browser->expandTopLevelDatabase(name);
}

void ConnectionTab::selectBrowserItem(const QString &path)
{
    m_browser->selectTreeItem(path);
}

void ConnectionTab::clickBrowserItem(const QString &path)
{
    m_browser->clickTreeItem(path);
}

void ConnectionTab::collapseBrowserItem(const QString &path)
{
    m_browser->collapseTreeItem(path);
}

void ConnectionTab::expandBrowserItem(const QString &path)
{
    m_browser->expandTreeItem(path);
}

QStringList ConnectionTab::dumpBrowserSubtree(const QString &path)
{
    return m_browser->dumpSubtree(path);
}

/* --sql= selftest: the active editor's text, caret at the end */
void ConnectionTab::setEditorTextForTest(const QString &sql)
{
    if(SqlEditor *ed = currentEditor()) {
        ed->setPlainText(sql);
        ed->moveCursorToEnd();
    }
}

/* --completeat= selftest: what Ctrl+Space offers with `sql` in the active
 * editor and the caret at its end — against this connection's real schema */
QStringList ConnectionTab::completionsForTest(const QString &sql)
{
    SqlEditor *ed = currentEditor();
    if(!ed)
        return {};
    setEditorTextForTest(sql);
    ed->triggerCompletion();
    return ed->completionHitsForTest();
}

QStringList ConnectionTab::dumpEditorText()
{
    SqlEditor *ed = currentEditor();
    if(!ed)
        return {};
    return ed->text().split(QLatin1Char('\n'));
}

QStringList ConnectionTab::browserContextMenuItems(const QString &path)
{
    return m_browser->contextMenuItemsForTest(path);
}

void ConnectionTab::doubleClickBrowserItem(const QString &path)
{
    m_browser->doubleClickTreeItem(path);
}

void ConnectionTab::promptFind()
{
    /* Ctrl+F searches where the cursor is: the Info page when focus is in it
     * (its search field always shows), else the query editor */
    if(QWidget *fw = QApplication::focusWidget(); fw && m_info->isAncestorOf(fw)) {
        m_info->focusSearch();
        return;
    }
    m_findBar->activate();
}

void ConnectionTab::findNext()
{
    m_findBar->findNext(false);
}

void ConnectionTab::promptReplace()
{
    auto *ed = currentEditor();
    if(!ed)
        return;
    bool ok = false;
    const QString from =
        QInputDialog::getText(this, QStringLiteral("Replace"), QStringLiteral("Find what:"),
                              QLineEdit::Normal, m_lastFind, &ok);
    if(!ok || from.isEmpty())
        return;
    const QString to =
        QInputDialog::getText(this, QStringLiteral("Replace"), QStringLiteral("Replace with:"),
                              QLineEdit::Normal, QString(), &ok);
    if(!ok)
        return;
    m_lastFind = from;
    QString text = ed->toPlainText();
    const int n = text.count(from);
    if(n == 0) {
        emit executed(QStringLiteral("Replace: \"%1\" not found").arg(from));
        return;
    }
    text.replace(from, to);
    ed->selectAll();
    ed->replaceSelectedText(text);
    emit executed(QStringLiteral("Replaced %1 occurrence(s)").arg(n));
}

void ConnectionTab::promptGoto()
{
    auto *ed = currentEditor();
    if(!ed)
        return;
    bool ok = false;
    int curLine = 0, curIndex = 0;
    ed->getCursorPosition(&curLine, &curIndex);
    const int line =
        QInputDialog::getInt(this, QStringLiteral("Go To Line"), QStringLiteral("Line number:"),
                             curLine + 1, 1, ed->lines(), 1, &ok);
    if(ok)
        ed->gotoLine(line);
}

void ConnectionTab::commentSelection(bool add)
{
    if(auto *ed = currentEditor())
        ed->toggleLineComment(add);
}

void ConnectionTab::listTags()
{
    if(auto *ed = currentEditor()) {
        ed->setFocus();
        ed->triggerCompletion();
    }
}

void ConnectionTab::formatQuery(int scope)
{
    auto *ed = currentEditor();
    if(!ed)
        return;
    if(scope == 2) { /* whole editor */
        ed->selectAll();
        ed->replaceSelectedText(SqlFormat::pretty(ed->toPlainText()));
        return;
    }
    if(scope == 1 && ed->hasSelectedText()) { /* selection */
        ed->replaceSelectedText(SqlFormat::pretty(ed->selectedText()));
        return;
    }
    /* current statement: expand to the surrounding ';' boundaries */
    const QString all = ed->toPlainText();
    const int pos = ed->cursorPosition();
    int start = all.lastIndexOf(';', qMax(0, pos - 1)) + 1;
    int end = all.indexOf(';', pos);
    if(end < 0)
        end = all.size();
    else
        ++end; /* include the ';' */
    int l0 = 0, i0 = 0, l1 = 0, i1 = 0;
    ed->getLineIndex(start, &l0, &i0);
    ed->getLineIndex(end, &l1, &i1);
    ed->setSelection(l0, i0, l1, i1);
    ed->replaceSelectedText(SqlFormat::pretty(all.mid(start, end - start)));
}

void ConnectionTab::editTableCell(int row, int col, const QString &value, bool stageOnly)
{
    if(stageOnly)
        m_tableData->stageCellOnly(row, col, value);
    else
        m_tableData->editCell(row, col, value);
}

void ConnectionTab::openSelectedTable()
{
    const QStringList info = currentTableInfo();
    if(info.size() < 2 || !m_conn)
        return;
    openTableData(info[0], info[1]);
}

QString ConnectionTab::cellTextForTest(int row, int col) const
{
    return m_tableData->cellTextForTest(row, col);
}

QString ConnectionTab::tdMenuDumpForTest(int row, int col)
{
    return m_tableData->contextMenuDumpForTest(row, col);
}

/* Loads a table into the Table Data grid through the right connection.
 * physDb routes PostgreSQL's multi-database tree: a table under a
 * non-primary database loads through that database's own (pooled) side
 * connection, not m_conn. connectionFor() falls back to m_conn for an empty
 * physDb (MySQL/SQLite) or the current primary, and pool entries are only
 * ever moved between m_conn and the pool — never deleted — so the pointer
 * stays valid for this tab's life. `activate` brings the tab to the front. */
void ConnectionTab::loadTableData(const QString &db, const QString &table, const QString &physDb,
                                  bool activate)
{
    if(!m_conn)
        return;
    QString error;
    IDbConnection *c = connectionFor(physDb, &error);
    if(!c) {
        m_messages->setPlainText(
            QStringLiteral("Could not open %1.%2: %3").arg(physDb, table, error));
        m_resultTabs->setCurrentWidget(m_messages);
        return;
    }
    m_tableData->load(c, db, table);
    m_loadedTableKey = QStringList{physDb, db, table}.join(QLatin1Char('\x1f'));
    /* remember whose connection is feeding the grid — primary (empty) or a
     * foreign database's side connection — so the status bar can say which
     * one the rows on screen came from */
    m_tableDataPhysDb = (c == m_conn) ? QString() : physDb;
    if(activate)
        m_resultTabs->setCurrentWidget(m_tableData);
    updateActiveConnectionLabel();
}

void ConnectionTab::openTableData(const QString &db, const QString &table)
{
    if(!m_conn)
        return;
    m_tableData->load(m_conn, db, table);
    m_resultTabs->setCurrentWidget(m_tableData);
}

void ConnectionTab::setDataViewMode(const QString &mode)
{
    if(mode.startsWith(QStringLiteral("check:"))) {
        m_tableData->checkRowsForTest(mode.mid(6));
        return;
    }
    if(mode.startsWith(QStringLiteral("hex:"))) {
        const QStringList p = mode.mid(4).split(QLatin1Char(':'));
        if(p.size() == 3)
            m_tableData->hexCellForTest(p[0].toInt(), p[1].toInt(), p[2]);
        return;
    }
    if(mode.startsWith(QStringLiteral("form"))) {
        m_tableData->formTestCommand(mode);
        return;
    }
    if(mode.startsWith(QStringLiteral("jump:"))) {
        m_tableData->formTestCommand(mode); /* jump:<row>:<col> lives there */
        return;
    }
    if(mode.startsWith(QStringLiteral("quickfilter:"))) {
        m_tableData->quickFilterForTest(mode.mid(QStringLiteral("quickfilter:").size()));
        return;
    }
    if(mode.startsWith(QStringLiteral("setdefault:"))) {
        const QStringList p =
            mode.mid(QStringLiteral("setdefault:").size()).split(QLatin1Char(':'));
        if(p.size() == 2)
            m_tableData->setCellDefaultForTest(p[0].toInt(), p[1].toInt());
        return;
    }
    m_tableData->setViewMode(mode == QStringLiteral("text") ? 2 : 0);
}

bool ConnectionTab::execDdl(const QString &sql)
{
    if(!m_conn)
        return false;
    QString error;
    bool ok;
    if(m_params.driverType == SqlDriverType::Sqlite) {
        /* sqlite3_prepare_v2 (see SqliteConnection::runBuffered) only ever
         * prepares the FIRST statement in a string and silently discards
         * anything after it — unlike PQexec, which runs a whole
         * semicolon-joined string as one implicit transaction. A few
         * dialogs (e.g. CreateTableDialog's SQLite Alter Table path) build
         * several ';'-separated statements the same way the Postgres path
         * does, so split and run each one separately here rather than at
         * every call site — wrapped in an explicit transaction when there's
         * more than one, so a mid-sequence failure doesn't leave the
         * earlier statements applied (matching PQexec's atomicity). */
        const QStringList stmts = splitStatements(sql, SqlDriverType::Sqlite);
        ok = true;
        const bool multi = stmts.size() > 1;
        if(multi)
            m_conn->query(QStringLiteral("BEGIN"), nullptr, nullptr);
        for(const QString &s : stmts) {
            if(s.trimmed().isEmpty())
                continue;
            QString stmtErr;
            if(!m_conn->query(s, nullptr, &stmtErr)) {
                error = stmtErr;
                ok = false;
                break;
            }
        }
        if(multi)
            m_conn->query(ok ? QStringLiteral("COMMIT") : QStringLiteral("ROLLBACK"), nullptr,
                          nullptr);
    } else {
        ok = m_conn->query(sql, nullptr, &error);
    }
    m_messages->appendPlainText(ok ? QStringLiteral("OK: ") + sql
                                   : QStringLiteral("Error: ") + error);
    m_resultTabs->setCurrentWidget(m_messages);
    if(ok)
        refreshBrowser();
    return ok;
}

QStringList ConnectionTab::currentTableInfo() const
{
    return m_browser->currentTableInfo();
}

QStringList ConnectionTab::selectedTableInfo() const
{
    return m_browser->currentTableInfo();
}

/* kinds follow Table > Paste SQL Statement (upstream ID_OBJECT_*STMT) */
void ConnectionTab::pasteSqlTemplate(int kind)
{
    const QStringList info = currentTableInfo();
    if(info.size() < 2)
        return;
    const QString db = info[0], table = info[1];
    const auto qi = [&](const QString &ident) {
        return m_conn ? m_conn->quoteIdent(ident) : QStringLiteral("`%1`").arg(ident);
    };

    QStringList cols;
    if(m_conn)
        for(const QStringList &row : m_conn->listColumns(db, table).rows)
            cols << row.value(0);
    if(cols.isEmpty())
        return;

    QStringList qCols;
    for(const QString &c : cols)
        qCols << qi(c);
    const QString colsB = qCols.join(QStringLiteral(", "));
    const QString tbl = qi(db) + QLatin1Char('.') + qi(table);
    QString stmt;
    switch(kind) {
        case 0: {
            QStringList marks;
            for(int i = 0; i < cols.size(); ++i)
                marks << QStringLiteral("?");
            stmt = QStringLiteral("INSERT INTO %1 (%2)\nVALUES (%3);")
                       .arg(tbl, colsB, marks.join(", "));
            break;
        }
        case 1: {
            QStringList sets;
            for(const QString &c : qCols)
                sets << QStringLiteral("%1 = '?'").arg(c);
            stmt = QStringLiteral("UPDATE %1 SET %2\nWHERE <condition>;").arg(tbl, sets.join(", "));
            break;
        }
        case 2:
            stmt = QStringLiteral("DELETE FROM %1\nWHERE <condition>;").arg(tbl);
            break;
        default:
            stmt = QStringLiteral("SELECT %1\nFROM %2;").arg(colsB, tbl);
    }
    if(auto *ed = currentEditor())
        ed->setPlainText(stmt);
    m_editorTabs->setCurrentIndex(0);
}

void ConnectionTab::toggleBrowserPane()
{
    m_browser->setVisible(!m_browser->isVisible());
}

void ConnectionTab::toggleResultPane()
{
    m_resultTabs->setVisible(!m_resultTabs->isVisible());
}

void ConnectionTab::toggleEditorPane()
{
    m_editorTabs->setVisible(!m_editorTabs->isVisible());
}

/* PostgreSQL: a database with exactly one schema has nothing to choose, so
 * make it current right away — the toolbar combo then shows it without the
 * user having to click the schema. Silent (no Messages text): this is part
 * of connecting/switching, not a user action. */
void ConnectionTab::selectSoleSchema()
{
    if(!m_conn || m_params.driverType != SqlDriverType::Postgres || m_databases.size() != 1 ||
       !m_currentSchema.isEmpty())
        return;
    const QString schema = m_databases.first();
    if(!m_conn->query(QStringLiteral("SET search_path TO %1").arg(m_conn->quoteIdent(schema)),
                      nullptr, nullptr))
        return;
    m_currentSchema = schema;
    updateCompletions();
}

void ConnectionTab::useDatabase(const QString &db)
{
    if(!m_conn)
        return;
    /* the toolbar combo's items are listDatabases()'s output, which for
     * PostgreSQL means schemas (see PostgresConnection.h) — switching to
     * one there is SET search_path, tracked in m_currentSchema, not
     * m_params.database (the connected database — still needed as-is for
     * reconnects/Copy Connection/Session save). SQLite has no equivalent
     * of switching the "current" attached database via a single statement
     * either way, so it's left on the MySQL-shaped USE below, same as
     * before — that surfaces a clear syntax-error message there rather
     * than silently doing nothing. */
    /* already current: nothing to run, but still emit below — the toolbar
     * combo can be stale (e.g. blank after switchDatabase()) even though
     * this schema was picked before; also keeps the click+double-click
     * pair on one tree node from issuing the same SET/USE twice */
    const QString alreadyCurrent =
        m_params.driverType == SqlDriverType::Postgres ? m_currentSchema : m_params.database;
    if(db != alreadyCurrent) {
        QString error;
        const bool ok =
            m_params.driverType == SqlDriverType::Postgres
                ? m_conn->query(QStringLiteral("SET search_path TO %1").arg(m_conn->quoteIdent(db)),
                                nullptr, &error)
                : m_conn->query(QStringLiteral("USE `%1`").arg(db), nullptr, &error);
        if(!ok) {
            m_messages->setPlainText(error);
            m_resultTabs->setCurrentWidget(m_messages);
            return;
        }
        if(m_params.driverType == SqlDriverType::Postgres)
            m_currentSchema = db;
        else
            m_params.database = db;
        m_messages->setPlainText(m_params.driverType == SqlDriverType::Postgres
                                     ? QStringLiteral("Schema changed to %1").arg(db)
                                     : QStringLiteral("Database changed to %1").arg(db));
        m_resultTabs->setCurrentWidget(m_messages);
        updateCompletions();
    }
    /* MainWindow re-syncs its toolbar combo off this signal — without it,
     * picking a schema from the tree left the combo showing the previous
     * (or blank) selection until the user happened to switch tabs */
    emit databasesChanged(m_databases, defaultDb());
}
