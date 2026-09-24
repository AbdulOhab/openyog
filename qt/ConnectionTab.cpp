#include "ConnectionTab.h"
#include "ObjectBrowser.h"
#include "TableDataView.h"

#include "CreateTableDialog.h"
#include "ExportDialog.h"
#include "ResultExport.h"
#include "SchemaSql.h"
#include "SqlSplit.h"
#include "FindBar.h"
#include "SqlFormat.h"
#include "UserManagerDialog.h"
#include "IndexDialog.h"
#include "ForeignKeyDialog.h"
#include "SqlDump.h"
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

/* DbResultSet renders a SQL NULL as the literal string "NULL" (matches the
 * existing QueryModel/result-grid convention); callers that need "no value"
 * semantics instead (e.g. an optional column default fed into generated DDL)
 * must convert back — this undoes that sentinel where it matters. */
QString orEmpty(const QString &v)
{
    return v == QStringLiteral("NULL") ? QString() : v;
}

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

/* charset picker for the import dialogs — MySQL/MariaDB charset names */
QComboBox *importCharsetCombo(QWidget *p)
{
    auto *c = new QComboBox(p);
    c->addItems({QStringLiteral("utf8mb4"), QStringLiteral("utf8"), QStringLiteral("latin1"),
                 QStringLiteral("cp1250"), QStringLiteral("cp1251"), QStringLiteral("cp1252"),
                 QStringLiteral("utf16"), QStringLiteral("utf16le"), QStringLiteral("ascii"),
                 QStringLiteral("big5"), QStringLiteral("gbk"), QStringLiteral("sjis"),
                 QStringLiteral("euckr")});
    c->setEditable(true);
    return c;
}

/* on-duplicate-key picker: LOAD DATA/XML take IGNORE or REPLACE */
QComboBox *importDupCombo(QWidget *p)
{
    auto *c = new QComboBox(p);
    c->addItem(QStringLiteral("IGNORE duplicate rows"), QStringLiteral("IGNORE"));
    c->addItem(QStringLiteral("REPLACE duplicate rows (by key)"), QStringLiteral("REPLACE"));
    return c;
}

/* read-only first-N-lines preview of the file being imported */
QPlainTextEdit *importFilePreview(QWidget *p, const QString &path, int lines = 15)
{
    auto *pv = new QPlainTextEdit(p);
    pv->setReadOnly(true);
    pv->setLineWrapMode(QPlainTextEdit::NoWrap);
    pv->setMaximumHeight(150);
    pv->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    QFile f(path);
    if(f.open(QIODevice::ReadOnly)) {
        QStringList ls;
        int n = 0;
        while(n++ < lines && !f.atEnd())
            ls << QString::fromUtf8(f.readLine());
        pv->setPlainText(ls.join(QString()).trimmed());
    }
    return pv;
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
            return b != QStringLiteral("NULL"); /* NULLs sort to the end */
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

/* minimal CSV/TSV-style parser: honours a quote char (with doubled-quote
 * escaping) and a separate escape char inside quotes, and treats both \n
 * and \r\n as a line end regardless of the dialog's "Line separator" pick
 * — used only by the SQLite import path, which has no server-side loader
 * to hand the raw separator/terminator strings to. */
QList<QStringList> parseDelimitedText(const QString &text, QChar sep, QChar quote, QChar esc)
{
    QList<QStringList> rows;
    QStringList cur;
    QString field;
    bool inQuotes = false;
    const int n = text.size();
    for(int i = 0; i < n;) {
        const QChar c = text[i];
        if(inQuotes) {
            if(!esc.isNull() && esc != quote && c == esc && i + 1 < n) {
                field += text[i + 1];
                i += 2;
                continue;
            }
            if(!quote.isNull() && c == quote) {
                if(i + 1 < n && text[i + 1] == quote) {
                    field += quote;
                    i += 2;
                    continue;
                }
                inQuotes = false;
                ++i;
                continue;
            }
            field += c;
            ++i;
            continue;
        }
        if(!quote.isNull() && c == quote) {
            inQuotes = true;
            ++i;
            continue;
        }
        if(c == sep) {
            cur << field;
            field.clear();
            ++i;
            continue;
        }
        if(c == QLatin1Char('\r')) {
            ++i;
            continue;
        }
        if(c == QLatin1Char('\n')) {
            cur << field;
            field.clear();
            rows << cur;
            cur.clear();
            ++i;
            continue;
        }
        field += c;
        ++i;
    }
    if(!field.isEmpty() || !cur.isEmpty()) {
        cur << field;
        rows << cur;
    }
    return rows;
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
    m_editor->setPlainText(
        m_params.driverType == SqlDriverType::Sqlite
            ? QStringLiteral("SELECT sqlite_version();\nSELECT name FROM sqlite_master;")
        : m_params.driverType == SqlDriverType::Postgres
            ? QStringLiteral("SELECT version(), current_user;\n"
                             "SELECT schema_name FROM information_schema.schemata;")
            : QStringLiteral("SELECT VERSION(), CURRENT_USER();\nSHOW DATABASES;"));
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

    m_info = new QTextBrowser(this);
    m_info->setOpenLinks(false);
    m_info->setHtml(
        QStringLiteral("<p style='color:#8a8a8a'>Select a table, view, procedure, function, "
                       "trigger, or event in the Object Browser to see its details here.</p>"));

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
    connect(m_resultTabs, &QTabWidget::currentChanged, this, [this](int) {
        if(m_resultTabs->currentWidget() == m_tableData)
            showPendingTable();
        updateActiveConnectionLabel();
    });

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

    /* a click shows the object in Info, and — for a table — in Table Data
     * too (SQLyog keeps both right-pane tabs on the selected object). The
     * grid is loaded lazily: only if Table Data is the tab in front, else
     * when the user switches to it, so browsing tables never runs a
     * SELECT nobody looks at. */
    connect(m_browser, &ObjectBrowser::objectSelected, this,
            [this](const QString &db, const QString &objType, const QString &name,
                   const QString &physDb) {
                updateInfoTab(db, objType, name, physDb);
                if(objType != QStringLiteral("TABLE"))
                    return;
                m_pendingTable = {db, name, physDb};
                if(m_resultTabs->currentWidget() == m_tableData)
                    showPendingTable();
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
    ed->setCompletions(m_completions);
    ed->setSchema(m_tableNames, m_columnNames);
    ed->setColumnLookup([this](const QString &table) {
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
    });
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

/* schema identifiers (tables + columns of the current db) for autocomplete —
 * kept split so the editor can offer tables after FROM/JOIN and columns after
 * SELECT/WHERE (clause-aware, like SQLyog's AutoCompleteInterface) */
void ConnectionTab::updateCompletions()
{
    m_completions.clear();
    m_columnCache.clear();
    m_tableNames.clear();
    m_columnNames.clear();
    const QString db = defaultDb();
    if(m_conn && !db.isEmpty()) {
        QSet<QString> tables;
        QStringList tbls = m_conn->listTables(db, QStringLiteral("BASE TABLE"));
        tbls += m_conn->listTables(db, QStringLiteral("VIEW"));
        for(const QString &t : tbls)
            if(!t.isEmpty())
                tables.insert(t);
        m_tableNames = QStringList(tables.cbegin(), tables.cend());
        /* one bulk query instead of a listColumns() round trip per table —
         * see IDbConnection::listAllColumnNames() */
        m_columnNames = m_conn->listAllColumnNames(db);
        QSet<QString> all = tables;
        for(const QString &c : m_columnNames)
            all.insert(c);
        m_completions = QStringList(all.cbegin(), all.cend());
    }
    for(int i = 0; i < m_editorTabs->count(); ++i)
        if(auto *ed = qobject_cast<SqlEditor *>(m_editorTabs->widget(i))) {
            ed->setCompletions(m_completions);
            ed->setSchema(m_tableNames, m_columnNames);
        }
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
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Backup `%1` as SQL dump").arg(table), table + QStringLiteral(".sql"),
        QStringLiteral("SQL (*.sql);;All (*)"));
    if(path.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    const bool ok = dumpDatabaseToFile(db, path, {table}, SqlDump::Options{}, &err);
    QApplication::restoreOverrideCursor();
    m_messages->setPlainText(ok ? QStringLiteral("Dumped `%1`.`%2` → %3").arg(db, table, path)
                                : QStringLiteral("Dump failed: %1").arg(err));
    m_resultTabs->setCurrentWidget(m_messages);
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
    const QString sql = ed->hasSelectedText()
                            ? ed->selectedText()
                            : statementAt(ed->toPlainText(), ed->cursorPosition());
    runStatements(splitStatements(sql), QStringLiteral("Execute Query"));
}

/* Ctrl+F9 — run the whole editor */
void ConnectionTab::runAll()
{
    if(SqlEditor *ed = currentEditor())
        runStatements(splitStatements(ed->toPlainText()), QStringLiteral("Execute Query"));
}

/* F8 — run the current statement, and if it's a single-table SELECT open that
 * table in the editable Table Data pane */
void ConnectionTab::runAndEdit()
{
    SqlEditor *ed = currentEditor();
    if(!ed)
        return;
    const QString stmt =
        (ed->hasSelectedText() ? ed->selectedText()
                               : statementAt(ed->toPlainText(), ed->cursorPosition()))
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
    QString stmt = ed->hasSelectedText() ? ed->selectedText()
                                         : statementAt(ed->toPlainText(), ed->cursorPosition());
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
            menu.addAction(QStringLiteral("&View Cell…"), grid, [this, grid, at] {
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

/* the table last clicked in the tree, loaded now that Table Data is in
 * front. Skipped when it's already the one on screen (keeps its sort/filter)
 * or when the grid holds staged edits — a click must never throw those away. */
void ConnectionTab::showPendingTable()
{
    if(m_pendingTable.table.isEmpty())
        return;
    const PendingTable t = m_pendingTable;
    m_pendingTable = {};
    if(m_tableData->hasStagedEdits())
        return;
    if(QStringList{t.physDb, t.db, t.table}.join(QLatin1Char('\x1f')) == m_loadedTableKey)
        return;
    loadTableData(t.db, t.table, t.physDb, false);
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
    m_tableData->setViewMode(mode == QStringLiteral("text") ? 2 : 0);
}

void ConnectionTab::promptCreateTable(const QString &database)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    CreateTableDialog dlg(db, this, m_params.driverType);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    if(sql.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Create Table"),
                             QStringLiteral("Nothing to create — a table name and at least "
                                            "one column are required."));
        return;
    }
    execDdl(sql); /* execDdl already refreshes the object browser on success */
}

void ConnectionTab::dropTable(const QString &database, const QString &table)
{
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(table.isEmpty() ||
       QMessageBox::question(this, QStringLiteral("Drop Table"),
                             QStringLiteral("Permanently DROP table `%1`.`%2`?").arg(db, table)) !=
           QMessageBox::Yes)
        return;
    execDdl(QStringLiteral("DROP TABLE %1").arg(m_conn->qualify(db, table)));
    if(m_tableData->loadedTable() == table)
        m_tableData->clear();
}

void ConnectionTab::truncateTable(const QString &database, const QString &table)
{
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(table.isEmpty() ||
       QMessageBox::question(this, QStringLiteral("Truncate Table"),
                             QStringLiteral("Delete ALL rows of `%1`.`%2`?").arg(db, table)) !=
           QMessageBox::Yes)
        return;
    execDdl(m_conn->sqlTruncateTable(db, table));
    if(m_tableData->loadedTable() == table)
        m_tableData->load(m_conn, db, table); /* empty grid */
}

/* ---- schema objects: View / Procedure / Function / Trigger / Event ------- */

void ConnectionTab::createSchemaObject(const QString &database, const QString &objType)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Create %1").arg(objType),
                             QStringLiteral("Select a database first."));
        return;
    }
    if(m_params.driverType == SqlDriverType::Postgres && objType == QStringLiteral("EVENT")) {
        QMessageBox::information(
            this, QStringLiteral("Create Event"),
            QStringLiteral("PostgreSQL has no built-in scheduled-event feature."));
        return;
    }
    const QString nice = objType.left(1) + objType.mid(1).toLower();
    /* SQLite has none of these three — no stored procedures, no CREATE
     * FUNCTION (user-defined functions there are registered through the C
     * API, not SQL DDL), no event scheduler. Without this guard,
     * SchemaSql::createTemplate() falls into its generic "MySQL/SQLite"
     * branch and opens an editor tab pre-filled with MySQL-only syntax
     * that just fails with a bare syntax error on Run — View and Trigger
     * are genuinely fine on SQLite and stay unguarded. */
    if(m_params.driverType == SqlDriverType::Sqlite &&
       (objType == QStringLiteral("PROCEDURE") || objType == QStringLiteral("FUNCTION") ||
        objType == QStringLiteral("EVENT"))) {
        QMessageBox::information(this, QStringLiteral("Create %1").arg(nice),
                                 QStringLiteral("SQLite has no stored procedures, functions, or "
                                                "scheduled events — only tables, views, indexes "
                                                "and triggers."));
        return;
    }
    /* SQLyog opens the DDL in a new query-editor tab, not a modal dialog */
    openEditorWithSql(
        QStringLiteral("Create %1").arg(nice),
        SchemaSql::editorText(objType, db, QString(),
                              SchemaSql::createTemplate(objType, db, m_params.driverType), true,
                              m_params.driverType));
}

void ConnectionTab::alterSchemaObject(const QString &database, const QString &objType,
                                      const QString &name)
{
    if(!m_conn || name.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    const QString nice = objType.left(1) + objType.mid(1).toLower();

    QString error;
    const QString ddl = m_conn->showCreate(objType, db, name, &error);
    if(ddl.isEmpty() && !error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Alter %1").arg(nice), error);
        return;
    }
    if(ddl.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Alter %1").arg(nice),
                             QStringLiteral("Could not read the object's DDL."));
        return;
    }
    openEditorWithSql(QStringLiteral("Alter %1 `%2`").arg(nice, name),
                      SchemaSql::editorText(objType, db, name, SchemaSql::stripDefiner(ddl), false,
                                            m_params.driverType));
}

void ConnectionTab::dropSchemaObject(const QString &database, const QString &objType,
                                     const QString &name)
{
    if(!m_conn || name.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    const QString nice = objType.left(1) + objType.mid(1).toLower();
    if(QMessageBox::question(
           this, QStringLiteral("Drop %1").arg(nice),
           QStringLiteral("Permanently DROP %1 %2?").arg(nice, m_conn->qualify(db, name))) !=
       QMessageBox::Yes)
        return;
    /* PostgreSQL's DROP TRIGGER needs "ON table", not a db-qualified name —
     * pulled from pg_trigger since dropSchemaObject() has no table
     * parameter of its own to pass in */
    if(m_params.driverType == SqlDriverType::Postgres && objType == QStringLiteral("TRIGGER")) {
        DbResultSet rs;
        m_conn->query(
            QStringLiteral("SELECT c.relname FROM pg_trigger t "
                           "JOIN pg_class c ON c.oid = t.tgrelid "
                           "JOIN pg_namespace n ON n.oid = c.relnamespace "
                           "WHERE NOT t.tgisinternal AND n.nspname = '%1' AND t.tgname = '%2'")
                .arg(db, name),
            &rs, nullptr);
        if(rs.rows.isEmpty()) {
            QMessageBox::warning(
                this, QStringLiteral("Drop Trigger"),
                QStringLiteral("Could not find the table this trigger belongs to."));
            return;
        }
        execDdl(QStringLiteral("DROP TRIGGER IF EXISTS %1 ON %2")
                    .arg(m_conn->quoteIdent(name), m_conn->qualify(db, rs.rows.first().value(0))));
        return;
    }
    execDdl(QStringLiteral("DROP %1 IF EXISTS %2").arg(objType, m_conn->qualify(db, name)));
}

/* ---- database-level operations ----------------------------------------- */

void ConnectionTab::dropDatabase(const QString &database)
{
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty())
        return;
    if(m_params.driverType == SqlDriverType::Sqlite) {
        /* a SQLite "database" IS the open file — there's no DROP DATABASE
         * statement, and dropping the file out from under the very
         * connection reading it isn't something a DDL call can safely do
         * anyway. Empty Database (drop every object, keep the file) is the
         * closest real equivalent; actually removing the file is a
         * filesystem operation the user does after closing the tab. */
        QMessageBox::information(this, QStringLiteral("Drop Database"),
                                 QStringLiteral("A SQLite connection's database is the open file "
                                                "itself — there's nothing to DROP while it's "
                                                "connected. Use Empty Database to drop every "
                                                "table/view/trigger and keep the file, or close "
                                                "this connection and delete the file."));
        return;
    }
    if(QMessageBox::warning(
           this, QStringLiteral("Drop Database"),
           QStringLiteral("Permanently DROP database `%1` and everything in it?").arg(db),
           QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    /* PostgreSQL: "database" here means schema (see PostgresConnection.h —
     * a connection can't reach another actual Postgres database at all,
     * let alone drop one), and there's no DROP DATABASE for that; CASCADE
     * is required since a non-empty schema otherwise refuses to drop. */
    execDdl(m_params.driverType == SqlDriverType::Postgres
                ? QStringLiteral("DROP SCHEMA %1 CASCADE").arg(m_conn->quoteIdent(db))
                : QStringLiteral("DROP DATABASE `%1`").arg(db));
}

void ConnectionTab::truncateDatabase(const QString &database)
{
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty())
        return;
    if(QMessageBox::warning(this, QStringLiteral("Truncate Database"),
                            QStringLiteral("DROP every table, view, routine, trigger and event "
                                           "in `%1`?  (the empty database is kept)")
                                .arg(db),
                            QMessageBox::Yes | QMessageBox::No,
                            QMessageBox::No) != QMessageBox::Yes)
        return;

    if(m_params.driverType == SqlDriverType::Postgres) {
        /* no per-schema charset/collation to preserve (that's a whole-
         * database property in Postgres) — just drop and recreate empty */
        const QString qdb = m_conn->quoteIdent(db);
        if(execDdl(QStringLiteral("DROP SCHEMA %1 CASCADE").arg(qdb)))
            execDdl(QStringLiteral("CREATE SCHEMA %1").arg(qdb));
        return;
    }
    if(m_params.driverType == SqlDriverType::Sqlite) {
        /* SQLite has no CREATE/DROP DATABASE at all — "truncate the
         * database, keep it empty" just means drop every object directly;
         * views first (harmless either order, but avoids a moment where a
         * view outlives the table it reads), then tables (dropping a table
         * also drops its own triggers/indexes automatically in SQLite) */
        for(const QString &v : m_conn->listTables(db, QStringLiteral("VIEW")))
            execDdl(QStringLiteral("DROP VIEW %1").arg(m_conn->qualify(db, v)));
        for(const QString &t : m_conn->listTables(db, QStringLiteral("BASE TABLE")))
            execDdl(QStringLiteral("DROP TABLE %1").arg(m_conn->qualify(db, t)));
        return;
    }

    /* read the current charset/collation so the recreated db keeps them */
    QString charset = QStringLiteral("utf8mb4"), collation;
    {
        DbResultSet rs;
        if(m_conn->query(QStringLiteral("SELECT DEFAULT_CHARACTER_SET_NAME, DEFAULT_COLLATION_NAME "
                                        "FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='%1'")
                             .arg(QString(db).replace('\'', QStringLiteral("''"))),
                         &rs, nullptr) &&
           !rs.rows.isEmpty()) {
            if(!orEmpty(rs.rows.first().value(0)).isEmpty())
                charset = rs.rows.first().value(0);
            collation = orEmpty(rs.rows.first().value(1));
        }
    }
    const QString bq = QString(db).replace('`', QStringLiteral("``"));
    QString create = QStringLiteral("CREATE DATABASE `%1` CHARACTER SET %2").arg(bq, charset);
    if(!collation.isEmpty())
        create += QStringLiteral(" COLLATE %1").arg(collation);
    if(execDdl(QStringLiteral("DROP DATABASE `%1`").arg(bq)))
        execDdl(create);
}

void ConnectionTab::emptyDatabase(const QString &database)
{
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty())
        return;
    if(QMessageBox::warning(
           this, QStringLiteral("Empty Database"),
           QStringLiteral("TRUNCATE every base table in `%1`?  All rows are lost.").arg(db),
           QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    const QStringList tables = m_conn->listTables(db, QStringLiteral("BASE TABLE"));
    execDdl(m_conn->sqlFkChecks(false));
    for(const QString &t : tables)
        execDdl(m_conn->sqlTruncateTable(db, t));
    execDdl(m_conn->sqlFkChecks(true));
    if(!m_tableData->loadedTable().isEmpty() && tables.contains(m_tableData->loadedTable()))
        m_tableData->load(m_conn, db, m_tableData->loadedTable());
}

void ConnectionTab::promptAlterDatabase(const QString &database)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty())
        return;
    if(m_params.driverType == SqlDriverType::Postgres) {
        QMessageBox::information(this, QStringLiteral("Alter Database"),
                                 QStringLiteral("Character set/collation are whole-database "
                                                "properties in PostgreSQL, fixed at creation — "
                                                "there's nothing here to alter for a schema."));
        return;
    }
    if(m_params.driverType == SqlDriverType::Sqlite) {
        QMessageBox::information(this, QStringLiteral("Alter Database"),
                                 QStringLiteral("SQLite has no per-database character set or "
                                                "collation to alter — text encoding is fixed "
                                                "(UTF-8/16) for the whole file at creation, and "
                                                "collations are attached per-column/-index, not "
                                                "to the database as a whole."));
        return;
    }

    QString curCharset, curCollation;
    {
        DbResultSet rs;
        if(m_conn->query(QStringLiteral("SELECT DEFAULT_CHARACTER_SET_NAME, DEFAULT_COLLATION_NAME "
                                        "FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='%1'")
                             .arg(QString(db).replace('\'', QStringLiteral("''"))),
                         &rs, nullptr) &&
           !rs.rows.isEmpty()) {
            curCharset = orEmpty(rs.rows.first().value(0));
            curCollation = orEmpty(rs.rows.first().value(1));
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Alter Database — `%1`").arg(db));
    auto *form = new QFormLayout(&dlg);
    auto *charsetEdit = new QLineEdit(curCharset, &dlg);
    auto *collationEdit = new QLineEdit(curCollation, &dlg);
    form->addRow(QStringLiteral("Character set"), charsetEdit);
    form->addRow(QStringLiteral("Collation"), collationEdit);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if(dlg.exec() != QDialog::Accepted)
        return;

    QString sql =
        QStringLiteral("ALTER DATABASE `%1`").arg(QString(db).replace('`', QStringLiteral("``")));
    if(!charsetEdit->text().trimmed().isEmpty())
        sql += QStringLiteral(" CHARACTER SET %1").arg(charsetEdit->text().trimmed());
    if(!collationEdit->text().trimmed().isEmpty())
        sql += QStringLiteral(" COLLATE %1").arg(collationEdit->text().trimmed());
    execDdl(sql);
}

void ConnectionTab::promptRenameTable(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Table"),
                                               QStringLiteral("New name for `%1`:").arg(table),
                                               QLineEdit::Normal, table, &ok);
    if(!ok || name.trimmed().isEmpty() || name == table)
        return;
    /* MySQL: RENAME TABLE db.old TO db.new. Neither PostgreSQL nor SQLite
     * has that statement — ALTER TABLE ... RENAME TO new does the same job
     * on both, within the same schema (there's no cross-schema form to
     * worry about since this function never changes db, only the name). */
    execDdl(
        m_params.driverType != SqlDriverType::Mysql
            ? QStringLiteral("ALTER TABLE %1 RENAME TO %2")
                  .arg(m_conn->qualify(db, table), m_conn->quoteIdent(name.trimmed()))
            : QStringLiteral("RENAME TABLE `%1`.`%2` TO `%1`.`%3`").arg(db, table, name.trimmed()));
    if(m_tableData->loadedTable() == table)
        m_tableData->load(m_conn, db, name.trimmed());
}

void ConnectionTab::promptCopyTable(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString srcDb = database.isEmpty() ? defaultDb() : database;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Duplicate Table `%1`").arg(table));
    auto *name = new QLineEdit(table + QStringLiteral("_copy"), &dlg);
    auto *targetDb = new QComboBox(&dlg);
    targetDb->addItems(m_databases.isEmpty() ? QStringList{srcDb} : m_databases);
    targetDb->setCurrentText(srcDb);
    auto *wantStructure = new QCheckBox(QStringLiteral("Structure"), &dlg);
    auto *wantData = new QCheckBox(QStringLiteral("Data"), &dlg);
    wantStructure->setChecked(true);
    wantData->setChecked(true);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("New table name"), name);
    form->addRow(QStringLiteral("Target database"), targetDb);
    form->addRow(QString(), wantStructure);
    form->addRow(QString(), wantData);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Duplicate"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return;

    const QString tgt = name->text().trimmed();
    const QString tgtDb = targetDb->currentText();
    if(tgt.isEmpty())
        return;
    const QString src = m_conn->qualify(srcDb, table);
    const QString dst = m_conn->qualify(tgtDb, tgt);

    if(wantStructure->isChecked()) {
        bool ok;
        if(m_params.driverType == SqlDriverType::Postgres) {
            /* PostgreSQL needs the parenthesized LIKE-clause form, and
             * INCLUDING ALL to get the same completeness a bare MySQL LIKE
             * gives by default (otherwise only column definitions copy,
             * not indexes/defaults/constraints) */
            ok = execDdl(QStringLiteral("CREATE TABLE %1 (LIKE %2 INCLUDING ALL)").arg(dst, src));
        } else if(m_params.driverType == SqlDriverType::Sqlite) {
            /* SQLite has no LIKE clause at all — showCreate("TABLE") for
             * SQLite replays the table's own original CREATE TABLE text
             * (sqlite_master.sql, byte-for-byte), so retarget just its
             * table name to the destination and replay that. This copies
             * every column/default/constraint/CHECK the original DDL had,
             * but not secondary indexes — those are each their own
             * separate CREATE INDEX statement in sqlite_master, not part
             * of CREATE TABLE's own text, and aren't reconstructed here. */
            QString err;
            QString ddl = m_conn->showCreate(QStringLiteral("TABLE"), srcDb, table, &err);
            static const QRegularExpression nameRe(
                QStringLiteral("^(CREATE\\s+TABLE\\s+(?:IF\\s+NOT\\s+EXISTS\\s+)?)"
                               "(?:\"[^\"]+\"|`[^`]+`|\\[[^\\]]+\\]|\\w+)"),
                QRegularExpression::CaseInsensitiveOption);
            ddl.replace(nameRe, QStringLiteral("\\1") + dst);
            ok = !ddl.isEmpty() && execDdl(ddl);
            if(!ok)
                m_messages->appendPlainText(QStringLiteral("Duplicate Table failed: ") +
                                            (err.isEmpty() ? ddl : err));
        } else {
            ok = execDdl(QStringLiteral("CREATE TABLE %1 LIKE %2").arg(dst, src));
        }
        if(!ok)
            return;
    }
    if(wantData->isChecked())
        execDdl(QStringLiteral("INSERT INTO %1 SELECT * FROM %2").arg(dst, src));
}

void ConnectionTab::promptDropColumn(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    const QString qualified = m_conn->qualify(db, table);

    QStringList cols;
    for(const QStringList &row : m_conn->listColumns(db, table).rows)
        cols << row.value(0);
    if(cols.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Drop Column"),
                                 QStringLiteral("No columns found in %1.").arg(qualified));
        return;
    }
    bool ok = false;
    const QString col = QInputDialog::getItem(
        this, QStringLiteral("Drop Column"),
        QStringLiteral("Column to drop from %1:").arg(qualified), cols, 0, false, &ok);
    if(!ok || col.isEmpty())
        return;
    if(QMessageBox::question(this, QStringLiteral("Drop Column"),
                             QStringLiteral("Drop column `%1` from %2? This cannot be undone.")
                                 .arg(col, qualified)) != QMessageBox::Yes)
        return;
    execDdl(
        QStringLiteral("ALTER TABLE %1 DROP COLUMN %2").arg(qualified, m_conn->quoteIdent(col)));
}

void ConnectionTab::promptManageIndexes(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;

    QList<IndexDialog::IndexDef> indexes;
    const auto findIx = [&](const QString &n) -> IndexDialog::IndexDef * {
        for(auto &ix : indexes)
            if(ix.name == n)
                return &ix;
        return nullptr;
    };
    /* canonical SHOW INDEX shape: Non_unique(1) Key_name(2) Column_name(4) */
    for(const QStringList &row : m_conn->listIndexes(db, table).rows) {
        const QString name = row.value(2);
        const QString col = row.value(4);
        IndexDialog::IndexDef *ix = findIx(name);
        if(!ix) {
            IndexDialog::IndexDef nd;
            nd.name = name;
            nd.unique = row.value(1) == QStringLiteral("0");
            nd.primary = name == QStringLiteral("PRIMARY");
            indexes << nd;
            ix = &indexes.last();
        }
        ix->columns << col;
    }

    QStringList cols;
    for(const QStringList &row : m_conn->listColumns(db, table).rows)
        cols << row.value(0);

    IndexDialog dlg(db, table, indexes, cols, this, m_params.driverType);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    if(sql.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Manage Indexes"),
                                 QStringLiteral("No changes to apply."));
        return;
    }
    execDdl(sql);
}

void ConnectionTab::promptCopyDatabase(const QString &database)
{
    if(!m_conn)
        return;

    /* SQLite has no server/multi-database concept the MySQL dialog below
     * assumes (host/port/user, CREATE DATABASE, information_schema FKs) —
     * a SQLite "database" just *is* the file. "Copy" means: duplicate the
     * file (schema + data, in one shot — a plain file copy is not just an
     * equivalent to the MySQL path, it's more complete, since it carries
     * indexes/views/triggers with no per-object-type code needed), or
     * recreate just the schema (CREATE TABLE/VIEW from showCreate()) into
     * a fresh file when data isn't wanted. */
    if(m_params.driverType == SqlDriverType::Sqlite) {
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("Copy Database"));
        auto *path = new QLineEdit(m_params.filePath + QStringLiteral("_copy.sqlite"), &dlg);
        auto *browse = new QPushButton(QStringLiteral("Browse…"), &dlg);
        connect(browse, &QPushButton::clicked, &dlg, [&] {
            /* unlike the "open/create a file" browse button on the SQLite
             * connect tab, picking a target *here* really does mean
             * overwrite — keep QFileDialog's default confirm-overwrite
             * prompt instead of suppressing it. */
            const QString p = QFileDialog::getSaveFileName(
                &dlg, QStringLiteral("Copy Database As"), path->text(),
                QStringLiteral("SQLite database (*.sqlite *.db *.sqlite3)"));
            if(!p.isEmpty())
                path->setText(p);
        });
        auto *pathRow = new QHBoxLayout;
        pathRow->addWidget(path, 1);
        pathRow->addWidget(browse);
        auto *wantData = new QCheckBox(QStringLiteral("Copy table data"), &dlg);
        wantData->setChecked(true);
        auto *form = new QFormLayout;
        form->addRow(QStringLiteral("Copy to file"), pathRow);
        form->addRow(QString(), wantData);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Copy"));
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        auto *lay = new QVBoxLayout(&dlg);
        lay->addLayout(form);
        lay->addWidget(buttons);
        if(dlg.exec() != QDialog::Accepted)
            return;

        const QString target = path->text().trimmed();
        if(target.isEmpty() || target == m_params.filePath)
            return;
        /* the Browse… button's own save dialog already confirms an
         * overwrite, but a path typed directly into the field skips that —
         * ask here too before the upcoming QFile::remove() */
        if(QFile::exists(target) &&
           QMessageBox::question(this, QStringLiteral("Copy Database"),
                                 QStringLiteral("%1 already exists. Overwrite it?").arg(target)) !=
               QMessageBox::Yes)
            return;

        QApplication::setOverrideCursor(Qt::WaitCursor);
        QString err;
        const bool ok = copySqliteFileTo(target, wantData->isChecked(), &err);
        QApplication::restoreOverrideCursor();
        m_messages->setPlainText(ok ? QStringLiteral("Copied database to %1").arg(target)
                                    : QStringLiteral("Copy failed:\n%1").arg(err));
        m_resultTabs->setCurrentWidget(m_messages);
        return;
    }

    const QString srcDb = database.isEmpty() ? defaultDb() : database;
    if(srcDb.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Copy Database"),
                                 QStringLiteral("Select a database first."));
        return;
    }

    /* Postgres has no cross-database browsing at all (see the schema/
     * database design note elsewhere in this file) — a connection can
     * never reach a sibling physical database the way the "different
     * target host" path below assumes, so "Copy Database" for Postgres is
     * always a same-connection schema-to-schema copy; the target
     * host/port/user fields (meaningless here) are omitted entirely rather
     * than shown and silently ignored. */
    const bool isPg = m_params.driverType == SqlDriverType::Postgres;

    QDialog dlg(this);
    dlg.setWindowTitle(
        QStringLiteral("Copy %1 `%2`")
            .arg(isPg ? QStringLiteral("Schema") : QStringLiteral("Database"), srcDb));
    auto *tHost = new QLineEdit(m_params.host, &dlg);
    auto *tPort = new QSpinBox(&dlg);
    tPort->setRange(1, 65535);
    tPort->setValue(m_params.port);
    tPort->setLocale(QLocale::c());
    auto *tUser = new QLineEdit(m_params.user, &dlg);
    auto *tPass = new QLineEdit(m_params.password, &dlg);
    tPass->setEchoMode(QLineEdit::Password);
    auto *name = new QLineEdit(srcDb + QStringLiteral("_copy"), &dlg);
    auto *wantData = new QCheckBox(QStringLiteral("Copy table data"), &dlg);
    wantData->setChecked(true);
    auto *dropFirst =
        new QCheckBox(QStringLiteral("Drop target %1 first if it exists")
                          .arg(isPg ? QStringLiteral("schema") : QStringLiteral("database")),
                      &dlg);
    auto *wantRoutines =
        new QCheckBox(QStringLiteral("Also copy views, routines, triggers, events"), &dlg);
    wantRoutines->setChecked(true);
    auto *form = new QFormLayout;
    if(!isPg) {
        form->addRow(QStringLiteral("Target host"), tHost);
        form->addRow(QStringLiteral("Target port"), tPort);
        form->addRow(QStringLiteral("Target user"), tUser);
        form->addRow(QStringLiteral("Target password"), tPass);
    }
    form->addRow(isPg ? QStringLiteral("New schema name") : QStringLiteral("New database name"),
                 name);
    form->addRow(QString(), wantData);
    form->addRow(QString(), wantRoutines);
    form->addRow(QString(), dropFirst);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Copy"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(new QLabel(
        isPg ? QStringLiteral("Copies the schema within this connection "
                              "(CREATE TABLE … LIKE … INCLUDING ALL, plus foreign keys, "
                              "views, routines and triggers).")
             : QStringLiteral(
                   "Same host + port + user → fast CREATE … LIKE copy; a different target "
                   "streams a dump over a fresh connection. DEFINER clauses are stripped."),
        &dlg));
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return;

    const QString tgt = name->text().trimmed();
    if(tgt.isEmpty())
        return;
    const bool sameServer =
        isPg || (tHost->text().trimmed() == m_params.host && tPort->value() == m_params.port &&
                 tUser->text().trimmed() == m_params.user);
    if(sameServer && tgt == srcDb) {
        QMessageBox::information(
            this, QStringLiteral("Copy Database"),
            QStringLiteral("Target must differ from the source on the same %1.")
                .arg(isPg ? QStringLiteral("connection") : QStringLiteral("server")));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    bool ok;
    if(isPg) {
        ok = copyDatabaseToPostgres(srcDb, tgt, wantData->isChecked(), dropFirst->isChecked(),
                                    wantRoutines->isChecked(), &err);
    } else if(sameServer) {
        ok = copyDatabaseTo(srcDb, tgt, wantData->isChecked(), dropFirst->isChecked(),
                            wantRoutines->isChecked(), &err);
    } else {
        ConnectionParams tp;
        tp.host = tHost->text().trimmed();
        tp.user = tUser->text().trimmed();
        tp.password = tPass->text();
        tp.port = tPort->value();
        IDbConnection *dst = dbDriverFor(SqlDriverType::Mysql)->connect(tp, &err);
        if(!dst) {
            err = QStringLiteral("target connect failed: %1").arg(err);
            ok = false;
        } else {
            const QString tq = QString(tgt).replace('`', QStringLiteral("``"));
            if(dropFirst->isChecked())
                dst->query(QStringLiteral("DROP DATABASE IF EXISTS `%1`").arg(tq), nullptr,
                           nullptr);
            dst->query(QStringLiteral("CREATE DATABASE IF NOT EXISTS `%1` "
                                      "CHARACTER SET utf8mb4")
                           .arg(tq),
                       nullptr, nullptr);
            dst->query(QStringLiteral("USE `%1`").arg(tq), nullptr, nullptr);
            SqlDump::Options opt;
            opt.data = wantData->isChecked();
            opt.routines = wantRoutines->isChecked();
            ok = SqlDump::forEachStatement(
                m_conn, srcDb, {}, opt,
                [&](const QString &stmt) {
                    QString stmtError;
                    if(dst->query(stmt, nullptr, &stmtError))
                        return true;
                    err = QStringLiteral("%1\n  at: %2").arg(stmtError, stmt.left(120));
                    return false;
                },
                err.isEmpty() ? &err : nullptr);
            delete dst;
        }
    }
    QApplication::restoreOverrideCursor();

    m_messages->setPlainText(ok ? QStringLiteral("Copied `%1` → %2`%3`.")
                                      .arg(srcDb,
                                           sameServer ? QString()
                                                      : QStringLiteral("%1:%2/")
                                                            .arg(tHost->text().trimmed())
                                                            .arg(tPort->value()),
                                           tgt)
                                : QStringLiteral("Copy failed:\n%1").arg(err));
    m_resultTabs->setCurrentWidget(m_messages);
    refreshBrowser();
}

bool ConnectionTab::importCsvBatched(const QString &db, const QString &table, const QString &file,
                                     const QString &sep, const QString &quote, const QString &escCh,
                                     bool hasHeader, int extraSkipLines, bool truncateFirst,
                                     const QString &onDup, int *rowsInserted, QString *error)
{
    if(!m_conn) {
        if(error)
            *error = QStringLiteral("not connected");
        return false;
    }
    QFile f(file);
    if(!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if(error)
            *error = QStringLiteral("could not open %1").arg(file);
        return false;
    }
    const QString text = QString::fromUtf8(f.readAll());
    const QChar sepCh = sep.isEmpty() ? QChar(',') : sep.at(0);
    const QChar quoteCh = quote.isEmpty() ? QChar() : quote.at(0);
    const QChar escapeCh = escCh.isEmpty() ? QChar() : escCh.at(0);
    QList<QStringList> rows = parseDelimitedText(text, sepCh, quoteCh, escapeCh);

    int start = 0;
    QStringList colNames;
    if(hasHeader && !rows.isEmpty()) {
        colNames = rows.first();
        for(QString &c : colNames)
            c = c.trimmed();
        start = 1;
    }
    start += extraSkipLines;

    if(truncateFirst)
        m_conn->query(m_conn->sqlTruncateTable(db, table), nullptr, nullptr);

    QString colClause;
    if(!colNames.isEmpty()) {
        QStringList q;
        for(const QString &c : colNames)
            q << m_conn->quoteIdent(c);
        colClause = QStringLiteral(" (%1)").arg(q.join(QStringLiteral(", ")));
    }
    const auto qv = [&](const QString &v) {
        if(v == QStringLiteral("NULL"))
            return QStringLiteral("NULL");
        return QLatin1Char('\'') + QString::fromUtf8(m_conn->escape(v.toUtf8())) +
               QLatin1Char('\'');
    };
    const QString qualified = m_conn->qualify(db, table);

    /* SQLite: INSERT OR IGNORE/REPLACE INTO ... — the whole conflict policy
     * lives in the verb, no clause needed after VALUES(...).
     * PostgreSQL: has neither keyword — the equivalent is
     * INSERT INTO ... VALUES (...) ON CONFLICT ... , appended *after* the
     * tuple, and "DO UPDATE" (REPLACE's real meaning: overwrite the
     * existing row) needs an explicit conflict target and column list,
     * not just a bare keyword. Built from the table's primary key when the
     * header names it (listIndexes() is the same canonical shape used
     * throughout the seam); anything less certain — no header, or the PK
     * isn't fully present in the imported columns — falls back to
     * ON CONFLICT DO NOTHING (IGNORE's behavior) rather than guessing at
     * an update that could silently target the wrong row. */
    QString verb = QStringLiteral("INSERT");
    QString conflictClause;
    if(m_params.driverType == SqlDriverType::Sqlite) {
        verb = onDup == QStringLiteral("REPLACE") ? QStringLiteral("INSERT OR REPLACE")
                                                  : QStringLiteral("INSERT OR IGNORE");
    } else if(m_params.driverType == SqlDriverType::Postgres) {
        QStringList pkCols;
        for(const QStringList &row : m_conn->listIndexes(db, table).rows)
            if(row.value(2) == QStringLiteral("PRIMARY"))
                pkCols << row.value(4);
        const bool pkUsable = !pkCols.isEmpty() && !colNames.isEmpty() &&
                              std::all_of(pkCols.cbegin(), pkCols.cend(), [&](const QString &c) {
                                  return colNames.contains(c, Qt::CaseInsensitive);
                              });
        if(onDup == QStringLiteral("REPLACE") && pkUsable) {
            QStringList pkQ, setClauses;
            for(const QString &c : pkCols)
                pkQ << m_conn->quoteIdent(c);
            for(const QString &c : colNames)
                if(!pkCols.contains(c, Qt::CaseInsensitive))
                    setClauses << QStringLiteral("%1 = EXCLUDED.%1").arg(m_conn->quoteIdent(c));
            conflictClause = setClauses.isEmpty()
                                 ? QStringLiteral(" ON CONFLICT (%1) DO NOTHING")
                                       .arg(pkQ.join(QStringLiteral(", ")))
                                 : QStringLiteral(" ON CONFLICT (%1) DO UPDATE SET %2")
                                       .arg(pkQ.join(QStringLiteral(", ")),
                                            setClauses.join(QStringLiteral(", ")));
        } else {
            conflictClause = QStringLiteral(" ON CONFLICT DO NOTHING");
        }
    }

    m_conn->query(QStringLiteral("BEGIN"), nullptr, nullptr);
    int inserted = 0;
    for(int i = start; i < rows.size(); ++i) {
        const QStringList &row = rows.at(i);
        if(row.size() == 1 && row.first().isEmpty())
            continue; /* trailing blank line */
        QStringList vals;
        for(const QString &v : row)
            vals << qv(v);
        const QString sql =
            QStringLiteral("%1 INTO %2%3 VALUES (%4)%5")
                .arg(verb, qualified, colClause, vals.join(QStringLiteral(", ")), conflictClause);
        QString stmtErr;
        if(!m_conn->query(sql, nullptr, &stmtErr)) {
            m_conn->query(QStringLiteral("ROLLBACK"), nullptr, nullptr);
            if(error)
                *error = QStringLiteral("%1\n  at row %2: %3")
                             .arg(stmtErr)
                             .arg(i + 1)
                             .arg(sql.left(120));
            return false;
        }
        ++inserted;
    }
    m_conn->query(QStringLiteral("COMMIT"), nullptr, nullptr);
    if(rowsInserted)
        *rowsInserted = inserted;
    return true;
}

bool ConnectionTab::copySqliteFileTo(const QString &target, bool withData, QString *error)
{
    if(!m_conn || m_params.driverType != SqlDriverType::Sqlite || target.isEmpty()) {
        if(error)
            *error = QStringLiteral("bad source/target");
        return false;
    }
    QFile::remove(target);
    if(withData) {
        if(QFile::copy(m_params.filePath, target))
            return true;
        if(error)
            *error = QStringLiteral("file copy failed");
        return false;
    }
    ConnectionParams tp;
    tp.driverType = SqlDriverType::Sqlite;
    tp.filePath = target;
    IDbConnection *dst = dbDriverFor(SqlDriverType::Sqlite)->connect(tp, error);
    if(!dst)
        return false;
    bool ok = true;
    for(const QString &kind : {QStringLiteral("TABLE"), QStringLiteral("VIEW")}) {
        if(!ok)
            break;
        for(const QString &name : m_conn->listTables(
                QStringLiteral("main"),
                kind == QStringLiteral("TABLE") ? QStringLiteral("BASE TABLE") : kind)) {
            QString ddlErr;
            const QString ddl = m_conn->showCreate(kind, QStringLiteral("main"), name, &ddlErr);
            if(ddl.isEmpty() || !dst->query(ddl, nullptr, error)) {
                ok = false;
                if(error && error->isEmpty())
                    *error = ddlErr;
                break;
            }
        }
    }
    delete dst;
    return ok;
}

void ConnectionTab::promptCopyTableToHost(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    /* the target here is always a fresh MySQL connection (see the dialog's
     * own label below) and the structure statement below is whatever
     * m_conn->showCreate() hands back — the *source's own* dialect. That's
     * fine when the source is MySQL too, but a Postgres/SQLite CREATE
     * TABLE (double-quoted identifiers, SERIAL/TEXT type differences, no
     * ENGINE=/CHARSET= clause, etc.) sent to a MySQL server the way this
     * always did would just fail with a syntax error rather than copying
     * anything — untested and unguarded until this fix. Unlike Copy
     * Database (which grew three genuinely separate per-driver code
     * paths — copyDatabaseTo()/copyDatabaseToPostgres()/the SQLite branch
     * in promptCopyDatabase()), a real cross-dialect DDL translator for
     * this one is a bigger job than a bug fix; guarding it is the same
     * defensive choice already made everywhere else in this file (Flush,
     * Show, Set Autocommit, …) rather than sending SQL nobody asked for. */
    if(m_params.driverType != SqlDriverType::Mysql) {
        QMessageBox::information(this, QStringLiteral("Copy Table To Different Host"),
                                 QStringLiteral("This only supports a MySQL/MariaDB source right "
                                                "now — the target is always MySQL, and copying "
                                                "a %1 table's structure across dialects isn't "
                                                "implemented. Use Database ▸ Copy Database or "
                                                "Backup Table(s) As SQL Dump instead.")
                                     .arg(m_params.driverType == SqlDriverType::Postgres
                                              ? QStringLiteral("PostgreSQL")
                                              : QStringLiteral("SQLite")));
        return;
    }
    const QString srcDb = database.isEmpty() ? defaultDb() : database;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Copy Table `%1` To Different Host/Database").arg(table));
    /* driver is guaranteed MySQL past the guard above, so this can just
     * prefill from the source tab's own connection */
    auto *tHost = new QLineEdit(m_params.host, &dlg);
    auto *tPort = new QSpinBox(&dlg);
    tPort->setRange(1, 65535);
    tPort->setValue(m_params.port);
    tPort->setLocale(QLocale::c());
    auto *tUser = new QLineEdit(m_params.user, &dlg);
    auto *tPass = new QLineEdit(m_params.password, &dlg);
    tPass->setEchoMode(QLineEdit::Password);
    auto *tDb = new QLineEdit(srcDb, &dlg);
    auto *wantData = new QCheckBox(QStringLiteral("Copy table data"), &dlg);
    wantData->setChecked(true);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Target host"), tHost);
    form->addRow(QStringLiteral("Target port"), tPort);
    form->addRow(QStringLiteral("Target user"), tUser);
    form->addRow(QStringLiteral("Target password"), tPass);
    form->addRow(QStringLiteral("Target database"), tDb);
    form->addRow(QString(), wantData);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Copy"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(new QLabel(
        QStringLiteral("The table keeps its name on the target — copy, then rename there "
                       "if you need a different one. The target is always MySQL (there's "
                       "no \"host\" to speak of for a SQLite file)."),
        &dlg));
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return;

    const QString tgtDb = tDb->text().trimmed();
    if(tgtDb.isEmpty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    ConnectionParams tp;
    tp.driverType = SqlDriverType::Mysql;
    tp.host = tHost->text().trimmed();
    tp.port = tPort->value();
    tp.user = tUser->text().trimmed();
    tp.password = tPass->text();
    QString err;
    IDbConnection *dst = dbDriverFor(SqlDriverType::Mysql)->connect(tp, &err);
    bool ok = dst != nullptr;
    if(ok) {
        const QString tb = QString(tgtDb).replace('`', QStringLiteral("``"));
        dst->query(QStringLiteral("CREATE DATABASE IF NOT EXISTS `%1`").arg(tb), nullptr, nullptr);
        dst->query(QStringLiteral("USE `%1`").arg(tb), nullptr, nullptr);
        SqlDump::Options opt;
        opt.data = wantData->isChecked();
        opt.routines = false;
        ok = SqlDump::forEachStatement(
            m_conn, srcDb, {table}, opt,
            [&](const QString &stmt) {
                QString stmtErr;
                if(dst->query(stmt, nullptr, &stmtErr))
                    return true;
                err = QStringLiteral("%1\n  at: %2").arg(stmtErr, stmt.left(120));
                return false;
            },
            err.isEmpty() ? &err : nullptr);
    } else {
        err = QStringLiteral("target connect failed: %1").arg(err);
    }
    delete dst;
    QApplication::restoreOverrideCursor();
    m_messages->setPlainText(
        ok ? QStringLiteral("Copied `%1` to %2:%3/`%4`").arg(table, tp.host).arg(tp.port).arg(tgtDb)
           : QStringLiteral("Copy failed:\n%1").arg(err));
    m_resultTabs->setCurrentWidget(m_messages);
}

bool ConnectionTab::copyDatabaseTo(const QString &srcDb, const QString &tgtDb, bool withData,
                                   bool dropFirst, bool withRoutines, QString *error)
{
    if(!m_conn || srcDb.isEmpty() || tgtDb.isEmpty() || srcDb == tgtDb) {
        if(error)
            *error = QStringLiteral("bad source/target");
        return false;
    }
    const QString sb = QString(srcDb).replace('`', QStringLiteral("``"));

    /* one-row helper: run `sql`, return column `col` of the first row */
    const auto oneRow = [&](const QString &sql, int col) -> QString {
        DbResultSet rs;
        if(m_conn->query(sql, &rs, nullptr) && !rs.rows.isEmpty())
            return rs.rows.first().value(col);
        return {};
    };
    /* names from a single-column query */
    const auto nameList = [&](const QString &sql, int col) {
        QStringList out;
        DbResultSet rs;
        if(m_conn->query(sql, &rs, nullptr))
            for(const QStringList &row : rs.rows)
                out << row.value(col);
        return out;
    };
    static const QRegularExpression kDefiner(QStringLiteral("DEFINER=`[^`]*`@`[^`]*` "));
    const auto retarget = [&](QString ddl) {
        return ddl.remove(kDefiner).replace(QStringLiteral("`%1`.").arg(srcDb),
                                            QStringLiteral("`%1`.").arg(tgtDb));
    };

    const auto bq = [](QString s) { return s.replace('`', QStringLiteral("``")); };
    const QString tb = bq(tgtDb);

    /* base tables — a failing SHOW here (e.g. no such source db) must abort,
     * not silently "succeed" with an empty target */
    QStringList tables;
    {
        DbResultSet rs;
        if(!m_conn->query(
               QStringLiteral("SHOW FULL TABLES FROM `%1` WHERE Table_type='BASE TABLE'").arg(sb),
               &rs, error))
            return false;
        for(const QStringList &row : rs.rows)
            tables << row.value(0);
    }

    QStringList stmts;
    if(dropFirst)
        stmts << QStringLiteral("DROP DATABASE IF EXISTS `%1`").arg(tb);
    stmts << QStringLiteral("CREATE DATABASE IF NOT EXISTS `%1`").arg(tb);
    stmts << QStringLiteral("SET FOREIGN_KEY_CHECKS=0");
    for(const QString &t : std::as_const(tables)) {
        const QString tq = bq(t);
        stmts << QStringLiteral("CREATE TABLE `%1`.`%2` LIKE `%3`.`%2`").arg(tb, tq, sb);
        if(withData)
            stmts
                << QStringLiteral("INSERT INTO `%1`.`%2` SELECT * FROM `%3`.`%2`").arg(tb, tq, sb);
    }

    /* CREATE TABLE … LIKE does not carry FK constraints (MariaDB) — copy them
     * explicitly from information_schema once every table exists (FK checks
     * are off, so create order is irrelevant) */
    {
        struct Fk
        {
            QString name, tbl, refTbl, onDel, onUpd;
            QStringList cols, refCols;
        };
        QList<Fk> fks;
        DbResultSet rs;
        if(m_conn->query(
               QStringLiteral("SELECT k.CONSTRAINT_NAME, k.TABLE_NAME, k.COLUMN_NAME, "
                              "k.REFERENCED_TABLE_NAME, k.REFERENCED_COLUMN_NAME, "
                              "r.DELETE_RULE, r.UPDATE_RULE "
                              "FROM information_schema.KEY_COLUMN_USAGE k "
                              "JOIN information_schema.REFERENTIAL_CONSTRAINTS r "
                              "  ON r.CONSTRAINT_SCHEMA=k.CONSTRAINT_SCHEMA "
                              "  AND r.CONSTRAINT_NAME=k.CONSTRAINT_NAME "
                              "WHERE k.TABLE_SCHEMA='%1' AND k.REFERENCED_TABLE_NAME IS NOT NULL "
                              "ORDER BY k.CONSTRAINT_NAME, k.ORDINAL_POSITION")
                   .arg(sb),
               &rs, nullptr)) {
            for(const QStringList &row : rs.rows) {
                const QString name = row.value(0);
                Fk *f = nullptr;
                for(auto &e : fks)
                    if(e.name == name && e.tbl == row.value(1)) {
                        f = &e;
                        break;
                    }
                if(!f) {
                    fks << Fk{name,
                              row.value(1),
                              row.value(3),
                              row.value(5) == QStringLiteral("NULL") ? QStringLiteral("RESTRICT")
                                                                     : row.value(5),
                              row.value(6) == QStringLiteral("NULL") ? QStringLiteral("RESTRICT")
                                                                     : row.value(6),
                              {},
                              {}};
                    f = &fks.last();
                }
                f->cols << row.value(2);
                f->refCols << row.value(4);
            }
        }
        for(const Fk &f : std::as_const(fks)) {
            const auto btlist = [&](const QStringList &l) {
                QStringList o;
                for(const QString &c : l)
                    o << QStringLiteral("`%1`").arg(bq(c));
                return o.join(QStringLiteral(", "));
            };
            stmts << QStringLiteral("ALTER TABLE `%1`.`%2` ADD CONSTRAINT `%3` FOREIGN KEY (%4) "
                                    "REFERENCES `%1`.`%5` (%6) ON DELETE %7 ON UPDATE %8")
                         .arg(tb, bq(f.tbl), bq(f.name), btlist(f.cols), bq(f.refTbl),
                              btlist(f.refCols), f.onDel, f.onUpd);
        }
    }

    if(withRoutines) {
        stmts << QStringLiteral("USE `%1`").arg(tb);

        for(const QString &v : nameList(
                QStringLiteral("SHOW FULL TABLES FROM `%1` WHERE Table_type='VIEW'").arg(sb), 0)) {
            QString ddl =
                retarget(oneRow(QStringLiteral("SHOW CREATE VIEW `%1`.`%2`").arg(srcDb, v), 1));
            ddl.replace(QStringLiteral(" VIEW `%1` ").arg(v),
                        QStringLiteral(" VIEW `%1`.`%2` ").arg(tgtDb, v));
            if(!ddl.isEmpty())
                stmts << ddl;
        }

        /* procedures + functions */
        struct R
        {
            QString name, type;
        };
        QList<R> routines;
        {
            DbResultSet rs;
            if(m_conn->query(QStringLiteral("SELECT ROUTINE_NAME, ROUTINE_TYPE FROM "
                                            "information_schema.ROUTINES WHERE ROUTINE_SCHEMA='%1'")
                                 .arg(sb),
                             &rs, nullptr))
                for(const QStringList &row : rs.rows)
                    routines << R{row.value(0), row.value(1)};
        }
        for(const R &rt : std::as_const(routines)) {
            const bool proc = rt.type == QStringLiteral("PROCEDURE");
            QString ddl = retarget(
                oneRow(QStringLiteral("SHOW CREATE %1 `%2`.`%3`")
                           .arg(proc ? QStringLiteral("PROCEDURE") : QStringLiteral("FUNCTION"),
                                srcDb, rt.name),
                       2));
            ddl.replace(QStringLiteral("%1 `%2`").arg(proc ? QStringLiteral("PROCEDURE")
                                                           : QStringLiteral("FUNCTION"),
                                                      rt.name),
                        QStringLiteral("%1 `%2`.`%3`")
                            .arg(proc ? QStringLiteral("PROCEDURE") : QStringLiteral("FUNCTION"),
                                 tgtDb, rt.name));
            if(!ddl.isEmpty())
                stmts << ddl;
        }

        /* triggers: SHOW TRIGGERS = Trigger,Event,Table,Statement,Timing,… */
        {
            DbResultSet rs;
            if(m_conn->query(QStringLiteral("SHOW TRIGGERS FROM `%1`").arg(sb), &rs, nullptr))
                for(const QStringList &row : rs.rows)
                    stmts << QStringLiteral("CREATE TRIGGER `%1`.`%2` %3 %4 ON `%1`.`%5` "
                                            "FOR EACH ROW %6")
                                 .arg(tgtDb, row.value(0), row.value(4), row.value(1), row.value(2),
                                      row.value(3));
        }

        for(const QString &e : nameList(QStringLiteral("SHOW EVENTS FROM `%1`").arg(sb), 1)) {
            QString ddl =
                retarget(oneRow(QStringLiteral("SHOW CREATE EVENT `%1`.`%2`").arg(srcDb, e), 3));
            ddl.replace(QStringLiteral(" EVENT `%1` ").arg(e),
                        QStringLiteral(" EVENT `%1`.`%2` ").arg(tgtDb, e));
            if(!ddl.isEmpty())
                stmts << ddl;
        }
    }

    stmts << QStringLiteral("SET FOREIGN_KEY_CHECKS=1");

    bool ok = true;
    for(const QString &s : std::as_const(stmts)) {
        QString stmtError;
        if(!m_conn->query(s, nullptr, &stmtError)) {
            if(error)
                *error = QStringLiteral("%1\n  at: %2").arg(stmtError, s);
            ok = false;
            break;
        }
    }
    m_conn->query(QStringLiteral("SET FOREIGN_KEY_CHECKS=1"), nullptr, nullptr);
    /* the "USE `tgt`" statement left the browsing connection on the target db;
     * put it back on this tab's database */
    if(!m_params.database.isEmpty())
        m_conn->query(QStringLiteral("USE `%1`").arg(m_params.database), nullptr, nullptr);
    return ok;
}

bool ConnectionTab::copyDatabaseToPostgres(const QString &srcSchema, const QString &tgtSchema,
                                           bool withData, bool dropFirst, bool withRoutines,
                                           QString *error)
{
    if(!m_conn || srcSchema.isEmpty() || tgtSchema.isEmpty() || srcSchema == tgtSchema) {
        if(error)
            *error = QStringLiteral("bad source/target");
        return false;
    }
    const QString sq = m_conn->quoteIdent(srcSchema);
    const QString tq = m_conn->quoteIdent(tgtSchema);

    /* blanket-retargets any *qualified* reference the catalog printed with
     * the source schema (mirrors copyDatabaseTo()'s backtick db-prefix
     * replace) to the target instead — pg_get_viewdef/functiondef/
     * triggerdef only double-quote an identifier that actually needs it,
     * so a plain lowercase schema name like this comes back *unquoted*
     * (`oy_src.t`, not `"oy_src".t`) far more often than not; matching only
     * the quoted spelling would silently leave the copy's views/functions/
     * triggers pointing at the *original* schema's objects instead of the
     * copies just created (same quoted-vs-bare lesson as the trigger-table
     * regex fix in SchemaSql.cpp). \b keeps a schema name from matching
     * inside a longer one (oy_src vs oy_src2). */
    const QRegularExpression schemaRef(
        QStringLiteral("(?:\"%1\"|\\b%1\\b)\\.").arg(QRegularExpression::escape(srcSchema)));
    const auto retarget = [&](QString ddl) {
        return ddl.replace(schemaRef, QStringLiteral("%1.").arg(tq));
    };

    const QStringList tables = m_conn->listTables(srcSchema, QStringLiteral("BASE TABLE"));

    QStringList stmts;
    if(dropFirst)
        stmts << QStringLiteral("DROP SCHEMA IF EXISTS %1 CASCADE").arg(tq);
    stmts << QStringLiteral("CREATE SCHEMA IF NOT EXISTS %1").arg(tq);
    for(const QString &t : tables) {
        const QString tt = m_conn->quoteIdent(t);
        /* LIKE ... INCLUDING ALL carries columns, defaults, NOT NULL,
         * identity, indexes, constraints and comments — but never foreign
         * keys, regardless of INCLUDING ALL (a documented Postgres LIKE
         * limitation); those are added explicitly below. A legacy `serial`
         * column (as opposed to this app's own GENERATED ... AS IDENTITY,
         * which copies cleanly with its own independent sequence) is a
         * plain integer DEFAULT nextval('src.seq'::regclass) — LIKE carries
         * that expression text verbatim, so the copy's column keeps
         * drawing values from the *source* schema's sequence rather than
         * getting an independent one; not fixed up here (narrow, only
         * affects externally-created tables using the legacy style). */
        stmts << QStringLiteral("CREATE TABLE %1.%2 (LIKE %3.%2 INCLUDING ALL)").arg(tq, tt, sq);
        if(withData)
            stmts << QStringLiteral("INSERT INTO %1.%2 SELECT * FROM %3.%2").arg(tq, tt, sq);
    }

    /* foreign keys: not carried by LIKE, added once every table exists.
     * Unlike the MySQL branch above, this can't reuse
     * information_schema.KEY_COLUMN_USAGE's REFERENCED_TABLE_NAME/
     * REFERENCED_COLUMN_NAME columns — those are a MySQL-only extension to
     * that view, absent from the ANSI-standard (and Postgres's) version —
     * so this reads pg_constraint directly instead, unnesting conkey/confkey
     * together (WITH ORDINALITY keeps each FK's column pairs and multi-
     * column order intact, same as ORDER BY ORDINAL_POSITION did above). */
    {
        struct Fk
        {
            QString name, tbl, refTbl, onDel, onUpd;
            QStringList cols, refCols;
        };
        QList<Fk> fks;
        DbResultSet rs;
        if(m_conn->query(
               QStringLiteral(
                   "SELECT con.conname, cl.relname, refcl.relname, "
                   "  CASE con.confdeltype WHEN 'c' THEN 'CASCADE' WHEN 'n' THEN 'SET NULL' "
                   "    WHEN 'd' THEN 'SET DEFAULT' WHEN 'r' THEN 'RESTRICT' ELSE 'NO ACTION' END, "
                   "  CASE con.confupdtype WHEN 'c' THEN 'CASCADE' WHEN 'n' THEN 'SET NULL' "
                   "    WHEN 'd' THEN 'SET DEFAULT' WHEN 'r' THEN 'RESTRICT' ELSE 'NO ACTION' END, "
                   "  a.attname, af.attname "
                   "FROM pg_constraint con "
                   "JOIN pg_class cl ON cl.oid=con.conrelid "
                   "JOIN pg_namespace n ON n.oid=cl.relnamespace "
                   "JOIN pg_class refcl ON refcl.oid=con.confrelid "
                   "JOIN unnest(con.conkey, con.confkey) WITH ORDINALITY AS u(ck, cfk, ord) ON "
                   "true "
                   "JOIN pg_attribute a ON a.attrelid=con.conrelid AND a.attnum=u.ck "
                   "JOIN pg_attribute af ON af.attrelid=con.confrelid AND af.attnum=u.cfk "
                   "WHERE con.contype='f' AND n.nspname='%1' "
                   "ORDER BY con.conname, u.ord")
                   .arg(srcSchema),
               &rs, nullptr)) {
            for(const QStringList &row : rs.rows) {
                const QString name = row.value(0);
                Fk *f = nullptr;
                for(auto &e : fks)
                    if(e.name == name && e.tbl == row.value(1)) {
                        f = &e;
                        break;
                    }
                if(!f) {
                    fks << Fk{name, row.value(1), row.value(2), row.value(3), row.value(4), {}, {}};
                    f = &fks.last();
                }
                f->cols << row.value(5);
                f->refCols << row.value(6);
            }
        }
        for(const Fk &f : std::as_const(fks)) {
            const auto qlist = [&](const QStringList &l) {
                QStringList o;
                for(const QString &c : l)
                    o << m_conn->quoteIdent(c);
                return o.join(QStringLiteral(", "));
            };
            stmts << QStringLiteral("ALTER TABLE %1.%2 ADD CONSTRAINT %3 FOREIGN KEY (%4) "
                                    "REFERENCES %1.%5 (%6) ON DELETE %7 ON UPDATE %8")
                         .arg(tq, m_conn->quoteIdent(f.tbl), m_conn->quoteIdent(f.name),
                              qlist(f.cols), m_conn->quoteIdent(f.refTbl), qlist(f.refCols),
                              f.onDel, f.onUpd);
        }
    }

    /* the connection's schema before this call, so the "SET search_path"
     * done below for view/routine/trigger creation can be put back after */
    const QString savedSearchPath =
        m_currentSchema.isEmpty() ? QStringLiteral("public") : m_currentSchema;

    if(withRoutines) {
        /* narrow search_path to just the target for this section, mirroring
         * copyDatabaseTo()'s "USE `tgt`" line — see the retarget() comment */
        stmts << QStringLiteral("SET search_path TO %1").arg(tq);

        for(const QString &v : m_conn->listTables(srcSchema, QStringLiteral("VIEW"))) {
            QString err;
            const QString ddl =
                retarget(m_conn->showCreate(QStringLiteral("VIEW"), srcSchema, v, &err));
            if(!ddl.isEmpty())
                stmts << ddl;
        }

        for(const QString &kind : {QStringLiteral("FUNCTION"), QStringLiteral("PROCEDURE")}) {
            DbResultSet rs;
            if(m_conn->query(QStringLiteral("SELECT ROUTINE_NAME FROM information_schema.ROUTINES "
                                            "WHERE ROUTINE_SCHEMA='%1' AND ROUTINE_TYPE='%2'")
                                 .arg(srcSchema, kind),
                             &rs, nullptr)) {
                for(const QStringList &row : rs.rows) {
                    QString err;
                    const QString ddl =
                        retarget(m_conn->showCreate(kind, srcSchema, row.value(0), &err));
                    if(!ddl.isEmpty())
                        stmts << ddl;
                }
            }
        }

        /* trigger *functions* are ordinary pg_proc entries, already copied
         * by the FUNCTION loop above (Postgres has no separate "trigger
         * body" object) — this only needs the CREATE TRIGGER statements
         * themselves, which reference them by the now-copied name */
        for(const QString &tr : m_conn->listTriggers(srcSchema)) {
            QString err;
            const QString ddl =
                retarget(m_conn->showCreate(QStringLiteral("TRIGGER"), srcSchema, tr, &err));
            if(!ddl.isEmpty())
                stmts << ddl;
        }
    }

    bool ok = true;
    for(const QString &s : std::as_const(stmts)) {
        QString stmtError;
        if(!m_conn->query(s, nullptr, &stmtError)) {
            if(error)
                *error = QStringLiteral("%1\n  at: %2").arg(stmtError, s);
            ok = false;
            break;
        }
    }
    if(withRoutines)
        m_conn->query(
            QStringLiteral("SET search_path TO %1").arg(m_conn->quoteIdent(savedSearchPath)),
            nullptr, nullptr);
    return ok;
}

void ConnectionTab::promptUserManager()
{
    if(!m_conn)
        return;
    if(m_params.driverType == SqlDriverType::Sqlite) {
        QMessageBox::information(this, QStringLiteral("User Manager"),
                                 QStringLiteral("SQLite has no user/permission system — "
                                                "a database file's access is just filesystem "
                                                "permissions on the .sqlite file itself."));
        return;
    }
    UserManagerDialog(m_conn, this, m_params.driverType).exec();
}

void ConnectionTab::tableDiagnostics(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString qualified = m_conn->qualify(database, table);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Table Diagnostics — %1").arg(table));
    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(
        new QLabel(QStringLiteral("Run a maintenance statement on %1:").arg(qualified), &dlg));

    struct Op
    {
        QString label, mysqlSql, sqliteSql, pgSql, note, pgNote;
    };
    const QList<Op> ops = {
        {QStringLiteral("&Check"), QStringLiteral("CHECK TABLE %1").arg(qualified),
         QStringLiteral("PRAGMA integrity_check"), QString(),
         QStringLiteral(" (whole file, not just this table)"),
         QStringLiteral(" (no built-in SQL equivalent — pg_amcheck is a "
                        "separate command-line tool)")},
        {QStringLiteral("&Analyze"), QStringLiteral("ANALYZE TABLE %1").arg(qualified),
         QStringLiteral("ANALYZE %1").arg(m_conn->quoteIdent(table)),
         QStringLiteral("ANALYZE %1").arg(qualified), QString(), QString()},
        {QStringLiteral("&Optimize"), QStringLiteral("OPTIMIZE TABLE %1").arg(qualified),
         QStringLiteral("VACUUM"), QStringLiteral("VACUUM (ANALYZE) %1").arg(qualified),
         QStringLiteral(" (whole file, not just this table)"), QString()},
        {QStringLiteral("&Repair"), QStringLiteral("REPAIR TABLE %1").arg(qualified), QString(),
         QString(), QStringLiteral(" (no SQLite equivalent)"),
         QStringLiteral(" (no PostgreSQL equivalent — corruption there means "
                        "restoring from backup, not a table-level fix)")},
    };
    for(const Op &op : ops) {
        auto *row = new QHBoxLayout;
        auto *btn = new QPushButton(op.label, &dlg);
        const QString sql = m_params.driverType == SqlDriverType::Sqlite     ? op.sqliteSql
                            : m_params.driverType == SqlDriverType::Postgres ? op.pgSql
                                                                             : op.mysqlSql;
        const QString note = m_params.driverType == SqlDriverType::Sqlite     ? op.note
                             : m_params.driverType == SqlDriverType::Postgres ? op.pgNote
                                                                              : QString();
        btn->setEnabled(!sql.isEmpty());
        connect(btn, &QPushButton::clicked, &dlg, [this, &dlg, sql] {
            dlg.accept();
            runStatements(QStringList{sql}, QStringLiteral("Diagnostics"));
        });
        row->addWidget(btn);
        row->addWidget(new QLabel(note, &dlg));
        row->addStretch(1);
        lay->addLayout(row);
    }
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(close, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(close, &QDialogButtonBox::clicked, &dlg, &QDialog::reject);
    lay->addWidget(close);
    dlg.exec();
}

void ConnectionTab::showConnectionInfo()
{
    if(!m_conn)
        return;
    const QString msg =
        QStringLiteral("Driver: %1\nServer: %2\n%3: %4\nCurrent database: %5")
            .arg(dbDriverFor(m_params.driverType)->driverName(), m_conn->serverInfo(),
                 m_params.driverType == SqlDriverType::Sqlite ? QStringLiteral("File")
                                                              : QStringLiteral("Host"),
                 hostLabel(),
                 m_params.database.isEmpty() ? QStringLiteral("(none)") : m_params.database);
    QMessageBox::information(this, QStringLiteral("Connection Info"), msg);
}

void ConnectionTab::showTableProperties(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    const QString qualified = m_conn->qualify(db, table);
    QString msg = QStringLiteral("Table: %1\n").arg(qualified);

    if(m_params.driverType == SqlDriverType::Sqlite) {
        DbResultSet rs;
        if(m_conn->query(QStringLiteral("SELECT COUNT(*) FROM %1").arg(qualified), &rs, nullptr) &&
           !rs.rows.isEmpty())
            msg += QStringLiteral("Rows: %1\n").arg(rs.rows.first().value(0));
        msg += QStringLiteral("Columns: %1\n").arg(m_conn->listColumns(db, table).rows.size());
        const int idxCount = [&] {
            QSet<QString> names;
            for(const QStringList &row : m_conn->listIndexes(db, table).rows)
                names.insert(row.value(2));
            return names.size();
        }();
        msg += QStringLiteral("Indexes: %1\n").arg(idxCount);
        if(m_params.driverType == SqlDriverType::Sqlite) {
            const QFileInfo fi(m_params.filePath);
            msg += QStringLiteral("Database file size: %1 bytes\n").arg(fi.size());
        }
    } else if(m_params.driverType == SqlDriverType::Postgres) {
        msg += QStringLiteral("Columns: %1\n").arg(m_conn->listColumns(db, table).rows.size());
        {
            QSet<QString> names;
            for(const QStringList &row : m_conn->listIndexes(db, table).rows)
                names.insert(row.value(2));
            msg += QStringLiteral("Indexes: %1\n").arg(names.size());
        }
        DbResultSet rs;
        /* pg_class.reltuples is a planner estimate (exact after ANALYZE),
         * same "estimate" caveat SHOW TABLE STATUS's own Rows column has
         * for MySQL; pg_relation_size/pg_indexes_size split table vs index
         * bytes the same way Data_length/Index_length do below. */
        if(m_conn->query(
               QStringLiteral("SELECT c.reltuples::bigint, pg_relation_size(c.oid), "
                              "pg_indexes_size(c.oid) "
                              "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
                              "WHERE n.nspname = '%1' AND c.relname = '%2'")
                   .arg(QString(db).replace('\'', QStringLiteral("''")),
                        QString(table).replace('\'', QStringLiteral("''"))),
               &rs, nullptr) &&
           !rs.rows.isEmpty()) {
            const QStringList &row = rs.rows.first();
            msg += QStringLiteral("Rows (estimate): %1\n").arg(row.value(0));
            msg += QStringLiteral("Data length: %1 bytes\n").arg(row.value(1));
            msg += QStringLiteral("Index length: %1 bytes\n").arg(row.value(2));
        } else {
            msg += QStringLiteral("(could not read pg_class)\n");
        }
    } else {
        DbResultSet rs;
        const QString sb = QString(db).replace('`', QStringLiteral("``"));
        const QString tb = QString(table).replace('`', QStringLiteral("``"));
        if(m_conn->query(QStringLiteral("SHOW TABLE STATUS FROM `%1` LIKE '%2'").arg(sb, tb), &rs,
                         nullptr) &&
           !rs.rows.isEmpty()) {
            /* SHOW TABLE STATUS: Name,Engine,Version,Row_format,Rows,
             * Avg_row_length,Data_length,Max_data_length,Index_length,
             * Data_free,Auto_increment,Create_time,... */
            const QStringList &row = rs.rows.first();
            msg += QStringLiteral("Engine: %1\n").arg(row.value(1));
            msg += QStringLiteral("Rows (estimate): %1\n").arg(row.value(4));
            msg += QStringLiteral("Data length: %1 bytes\n").arg(row.value(6));
            msg += QStringLiteral("Index length: %1 bytes\n").arg(row.value(8));
            msg += QStringLiteral("Created: %1\n").arg(row.value(11));
        } else {
            msg += QStringLiteral("(could not read SHOW TABLE STATUS)\n");
        }
    }
    QMessageBox::information(this, QStringLiteral("Table Properties"), msg);
}

QString ConnectionTab::buildSchemaHtml(const QString &db)
{
    QString html =
        QStringLiteral(
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>Schema: %1</title><style>"
            "body{font-family:sans-serif;margin:24px}"
            "table{border-collapse:collapse;margin:8px 0 24px}"
            "th,td{border:1px solid #ccc;padding:4px 10px;font-size:13px;text-align:left}"
            "th{background:#eef3f8}h2{margin-top:32px;border-bottom:2px solid #3b7dbb}"
            "h3{margin-bottom:4px;color:#555}</style></head><body>"
            "<h1>Schema: %1</h1>")
            .arg(db.toHtmlEscaped());

    const auto section = [&](const QString &heading, const QString &typeFilter) {
        const QStringList names = m_conn->listTables(db, typeFilter);
        for(const QString &name : names) {
            html += QStringLiteral("<h2>%1: %2</h2>").arg(heading, name.toHtmlEscaped());
            html += QStringLiteral("<table><tr><th>Column</th><th>Type</th><th>Null</th>"
                                   "<th>Key</th><th>Default</th><th>Extra</th></tr>");
            for(const QStringList &row : m_conn->listColumns(db, name).rows) {
                html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td>"
                                       "<td>%4</td><td>%5</td><td>%6</td></tr>")
                            .arg(row.value(0).toHtmlEscaped(), row.value(1).toHtmlEscaped(),
                                 row.value(2).toHtmlEscaped(), row.value(3).toHtmlEscaped(),
                                 row.value(4).toHtmlEscaped(), row.value(5).toHtmlEscaped());
            }
            html += QStringLiteral("</table>");

            if(typeFilter == QStringLiteral("BASE TABLE")) {
                const DbResultSet ixs = m_conn->listIndexes(db, name);
                if(!ixs.rows.isEmpty()) {
                    html += QStringLiteral("<h3>Indexes</h3><table>"
                                           "<tr><th>Name</th><th>Unique</th><th>Column</th></tr>");
                    for(const QStringList &row : ixs.rows) {
                        html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td></tr>")
                                    .arg(row.value(2).toHtmlEscaped(),
                                         row.value(1) == QStringLiteral("0") ? QStringLiteral("yes")
                                                                             : QStringLiteral("no"),
                                         row.value(4).toHtmlEscaped());
                    }
                    html += QStringLiteral("</table>");
                }
            }
        }
    };
    section(QStringLiteral("Table"), QStringLiteral("BASE TABLE"));
    section(QStringLiteral("View"), QStringLiteral("VIEW"));
    html += QStringLiteral("</body></html>");
    return html;
}

void ConnectionTab::promptDataSearch(const QString &database)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Data Search"),
                                 QStringLiteral("Select a database first."));
        return;
    }
    bool ok = false;
    const QString term = QInputDialog::getText(
        this, QStringLiteral("Data Search"),
        QStringLiteral("Search every text column of %1 for (LIKE match):").arg(db),
        QLineEdit::Normal, QString(), &ok);
    if(!ok || term.trimmed().isEmpty())
        return;
    const QString esc = QString::fromUtf8(m_conn->escape(term.toUtf8()));
    const QString likeClause = QStringLiteral(" LIKE '%") + esc + QStringLiteral("%'");

    /* CHAR/TEXT-ish columns only — a heuristic on the type name, not a
     * portable type-category API, but good enough to skip numeric/date/
     * blob columns a LIKE search wouldn't meaningfully match anyway */
    QStringList unions;
    for(const QString &table : m_conn->listTables(db, QStringLiteral("BASE TABLE"))) {
        for(const QStringList &col : m_conn->listColumns(db, table).rows) {
            const QString type = col.value(1).toUpper();
            if(!type.contains(QStringLiteral("CHAR")) && !type.contains(QStringLiteral("TEXT")))
                continue;
            const QString colName = col.value(0);
            const QString qcol = m_conn->quoteIdent(colName);
            unions << QStringLiteral(
                          "SELECT '%1' AS match_table, '%2' AS match_column, %3 AS match_value "
                          "FROM %4 WHERE %5")
                          .arg(table, colName, qcol, m_conn->qualify(db, table), qcol + likeClause);
        }
    }
    if(unions.isEmpty()) {
        m_messages->setPlainText(QStringLiteral("No text columns found in %1 to search.").arg(db));
        m_resultTabs->setCurrentWidget(m_messages);
        return;
    }
    runStatements(QStringList{unions.join(QStringLiteral(" UNION ALL "))},
                  QStringLiteral("Search"));
}

void ConnectionTab::promptSchemaHtml(const QString &database)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Create Schema HTML"),
                                 QStringLiteral("Select a database first."));
        return;
    }
    const QString file =
        QFileDialog::getSaveFileName(this, QStringLiteral("Create Schema For Database In HTML"),
                                     db + QStringLiteral(".html"), QStringLiteral("HTML (*.html)"));
    if(file.isEmpty())
        return;

    const QString html = buildSchemaHtml(db);
    QFile f(file);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("Create Schema HTML"),
                             QStringLiteral("Could not write %1").arg(file));
        return;
    }
    f.write(html.toUtf8());
    QMessageBox::information(this, QStringLiteral("Create Schema HTML"),
                             QStringLiteral("Saved to %1").arg(file));
}

void ConnectionTab::exportCurrent()
{
    if(m_resultTabs->currentWidget() == m_tableData && !m_tableData->loadedTable().isEmpty())
        exportTableData(m_tableData->loadedDb(), m_tableData->loadedTable());
    else
        exportResult();
}

void ConnectionTab::exportTableData(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;

    /* structure, for the SQL "include CREATE TABLE" option */
    const QString createDdl = m_conn->showCreate(QStringLiteral("TABLE"), db, table, nullptr);

    /* all rows — re-query without the Table Data pane's LIMIT. Streamed (not
     * query()) so a huge table stops fetching at the cap instead of
     * buffering every row before we get a chance to cap the result. */
    QStringList headers;
    QVector<QStringList> rows;
    constexpr int kCap = 500000;
    QString error;
    const bool ok = m_conn->streamQuery(
        QStringLiteral("SELECT * FROM %1").arg(m_conn->qualify(db, table)), &error,
        [&](const QStringList &h) { headers = h; },
        [&](const QVector<QByteArray> &fields, const QVector<bool> &isNull) {
            QStringList r;
            for(int i = 0; i < fields.size(); ++i)
                r << (isNull[i] ? QStringLiteral("NULL") : QString::fromUtf8(fields[i]));
            rows << r;
            return rows.size() < kCap;
        });
    if(!ok) {
        QMessageBox::warning(this, QStringLiteral("Export Table Data"), error);
        return;
    }
    if(rows.size() >= kCap)
        m_messages->appendPlainText(QStringLiteral("note: export capped at %1 rows").arg(kCap));

    ExportDialog dlg(table, table, rows.size(), false, this);
    if(dlg.exec() != QDialog::Accepted || dlg.path().isEmpty())
        return;

    ResultExport::Options opt = dlg.options();
    opt.sqlStructure = dlg.includeStructure();
    opt.sqlCreate = createDdl;
    opt.driver = m_params.driverType;
    const auto cell = [&](int r, int c) { return rows.at(r).at(c); };
    QString err;
    if(ResultExport::write(dlg.path(), dlg.format(), headers, cell, rows.size(), headers.size(),
                           opt, &err))
        m_messages->appendPlainText(QStringLiteral("Exported %1 row(s) of `%2` → %3")
                                        .arg(rows.size())
                                        .arg(table, dlg.path()));
    else
        m_messages->appendPlainText(QStringLiteral("Export failed: ") + err);
    m_resultTabs->setCurrentWidget(m_messages);
}

void ConnectionTab::promptImportXml(const QString &database, const QString &table)
{
    if(!m_conn)
        return;
    if(m_params.driverType != SqlDriverType::Mysql) {
        QMessageBox::information(this, QStringLiteral("Import XML"),
                                 QStringLiteral("XML import needs LOAD XML LOCAL INFILE, which is "
                                                "MySQL-only — not available on this connection. "
                                                "Use Import CSV instead."));
        return;
    }
    const QString db = database.isEmpty() ? defaultDb() : database;

    const QString file =
        QFileDialog::getOpenFileName(this, QStringLiteral("Import XML — pick a file"), QString(),
                                     QStringLiteral("XML (*.xml);;All files (*)"));
    if(file.isEmpty())
        return;

    QStringList tbls = m_conn->listTables(db, QStringLiteral("BASE TABLE"));

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Import XML into `%1`").arg(db));
    auto *tbl = new QComboBox(&dlg);
    tbl->addItems(tbls);
    if(!table.isEmpty())
        tbl->setCurrentText(table);
    auto *rowTag = new QLineEdit(QStringLiteral("row"), &dlg);
    auto *charset = importCharsetCombo(&dlg);
    auto *onDup = importDupCombo(&dlg);
    auto *truncate = new QCheckBox(QStringLiteral("Empty the table first"), &dlg);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Target table"), tbl);
    form->addRow(QStringLiteral("Character set"), charset);
    form->addRow(QStringLiteral("Rows identified by  <tag>"), rowTag);
    form->addRow(QStringLiteral("On duplicate key"), onDup);
    form->addRow(QString(), truncate);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Import"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(new QLabel(QStringLiteral("File preview:"), &dlg));
    lay->addWidget(importFilePreview(&dlg, file));
    lay->addWidget(new QLabel(
        QStringLiteral("Uses LOAD XML LOCAL INFILE — the server must allow local-infile."), &dlg));
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted || tbl->currentText().isEmpty())
        return;

    const QString target = tbl->currentText();
    const auto esc = [](QString s) {
        return s.replace('\\', QStringLiteral("\\\\")).replace('\'', QStringLiteral("\\'"));
    };
    const QString tag =
        rowTag->text().trimmed().isEmpty() ? QStringLiteral("row") : rowTag->text().trimmed();
    if(truncate->isChecked())
        execDdl(m_conn->sqlTruncateTable(db, target));

    const QString sql = QStringLiteral("LOAD XML LOCAL INFILE '%1' %2 INTO TABLE `%3`.`%4` "
                                       "CHARACTER SET %5 ROWS IDENTIFIED BY '<%6>'")
                            .arg(esc(file), onDup->currentData().toString(), db, target,
                                 charset->currentText().trimmed(), esc(tag));
    QString xmlError;
    if(!m_conn->query(sql, nullptr, &xmlError)) {
        m_messages->setPlainText(QStringLiteral("XML import failed: %1").arg(xmlError));
    } else {
        const QString info = m_conn->info();
        m_messages->setPlainText(
            QStringLiteral("Imported into `%1`.`%2` — %3")
                .arg(db, target,
                     !info.isEmpty() ? info
                                     : QStringLiteral("%1 row(s)").arg(m_conn->affectedRows())));
        if(m_tableData->loadedTable() == target)
            m_tableData->load(m_conn, db, target);
    }
    m_resultTabs->setCurrentWidget(m_messages);
    refreshBrowser();
}

void ConnectionTab::promptImportCsv(const QString &database, const QString &table)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;

    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import CSV — pick a file"), QString(),
        QStringLiteral("CSV / text (*.csv *.tsv *.txt);;All files (*)"));
    if(file.isEmpty())
        return;

    /* target table + parse options */
    QStringList tbls = m_conn->listTables(db, QStringLiteral("BASE TABLE"));

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Import CSV into `%1`").arg(db));
    auto *tbl = new QComboBox(&dlg);
    tbl->addItems(tbls);
    if(!table.isEmpty())
        tbl->setCurrentText(table);
    auto *fieldSep = new QLineEdit(QStringLiteral(","), &dlg);
    auto *enclosure = new QLineEdit(QStringLiteral("\""), &dlg);
    auto *escChar = new QLineEdit(QStringLiteral("\\"), &dlg);
    auto *optEnclose = new QCheckBox(QStringLiteral("Quote character is optional"), &dlg);
    optEnclose->setChecked(true);
    auto *lineSep = new QComboBox(&dlg);
    lineSep->addItems({QStringLiteral("\\n  (Unix)"), QStringLiteral("\\r\\n  (Windows)")});
    auto *charset = importCharsetCombo(&dlg);
    auto *header = new QCheckBox(QStringLiteral("First line holds column names"), &dlg);
    header->setChecked(true);
    auto *skipLines = new QSpinBox(&dlg);
    skipLines->setRange(0, 100000000);
    skipLines->setToolTip(QStringLiteral("extra leading lines to skip, on top "
                                         "of the header"));
    skipLines->setLocale(QLocale::c());
    auto *truncate = new QCheckBox(QStringLiteral("Empty the table first"), &dlg);
    auto *onDup = importDupCombo(&dlg);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Target table"), tbl);
    form->addRow(QStringLiteral("Character set"), charset);
    form->addRow(QStringLiteral("Field separator"), fieldSep);
    form->addRow(QStringLiteral("Quote character"), enclosure);
    form->addRow(QString(), optEnclose);
    form->addRow(QStringLiteral("Escape character"), escChar);
    form->addRow(QStringLiteral("Line separator"), lineSep);
    form->addRow(QStringLiteral("Skip leading lines"), skipLines);
    form->addRow(QStringLiteral("On duplicate key"), onDup);
    form->addRow(QString(), header);
    form->addRow(QString(), truncate);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Import"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(new QLabel(QStringLiteral("File preview:"), &dlg));
    lay->addWidget(importFilePreview(&dlg, file));
    const bool noBulkLoader = m_params.driverType != SqlDriverType::Mysql;
    lay->addWidget(new QLabel(
        noBulkLoader
            ? QStringLiteral("Parsed and inserted row by row inside one transaction "
                             "(no server-side bulk loader on this backend).")
            : QStringLiteral("Uses LOAD DATA LOCAL INFILE — the server must allow local-infile."),
        &dlg));
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted || tbl->currentText().isEmpty())
        return;

    const QString target = tbl->currentText();
    const QString sep = fieldSep->text().isEmpty() ? QStringLiteral(",") : fieldSep->text();
    const QString quote = enclosure->text();
    const auto esc = [](QString s) {
        return s.replace('\\', QStringLiteral("\\\\")).replace('\'', QStringLiteral("\\'"));
    };
    const int skip = (header->isChecked() ? 1 : 0) + skipLines->value();

    if(noBulkLoader) {
        int rows = 0;
        QString err;
        const bool ok = importCsvBatched(
            db, target, file, sep, quote, escChar->text(), header->isChecked(), skipLines->value(),
            truncate->isChecked(), onDup->currentData().toString(), &rows, &err);
        m_messages->setPlainText(ok ? QStringLiteral("Imported %1 row(s) into %2")
                                          .arg(rows)
                                          .arg(m_conn->qualify(db, target))
                                    : QStringLiteral("Import failed: %1").arg(err));
        m_resultTabs->setCurrentWidget(m_messages);
        if(ok && m_tableData->loadedTable() == target)
            m_tableData->load(m_conn, db, target);
        refreshBrowser();
        return;
    }

    /* when the file has a header row, map by name — otherwise LOAD DATA loads
     * positionally into every column (wrong for an AUTO_INCREMENT-first table) */
    QString colList;
    if(header->isChecked()) {
        QFile f(file);
        if(f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            QStringList cols;
            for(QString c : line.split(sep)) {
                c = c.trimmed();
                if(!quote.isEmpty() && c.startsWith(quote) && c.endsWith(quote))
                    c = c.mid(quote.size(), c.size() - 2 * quote.size());
                cols << QStringLiteral("`%1`").arg(c.replace('`', QStringLiteral("``")));
            }
            if(!cols.isEmpty())
                colList = QStringLiteral(" (%1)").arg(cols.join(QStringLiteral(", ")));
        }
    }

    if(truncate->isChecked())
        execDdl(m_conn->sqlTruncateTable(db, target));

    const QString enclosedClause =
        quote.isEmpty()
            ? QString()
            : QStringLiteral(" %1ENCLOSED BY '%2'")
                  .arg(optEnclose->isChecked() ? QStringLiteral("OPTIONALLY ") : QString(),
                       esc(quote));
    const QString escClause = escChar->text().isEmpty()
                                  ? QString()
                                  : QStringLiteral(" ESCAPED BY '%1'").arg(esc(escChar->text()));

    QString sql =
        QStringLiteral("LOAD DATA LOCAL INFILE '%1' %2 INTO TABLE `%3`.`%4` "
                       "CHARACTER SET %5 "
                       "FIELDS TERMINATED BY '%6'%7%8 "
                       "LINES TERMINATED BY '%9'%10%11")
            .arg(esc(file), onDup->currentData().toString(), db, target,
                 charset->currentText().trimmed(), esc(sep), enclosedClause, escClause,
                 lineSep->currentIndex() == 1 ? QStringLiteral("\\r\\n") : QStringLiteral("\\n"),
                 skip > 0 ? QStringLiteral(" IGNORE %1 LINES").arg(skip) : QString(), colList);

    QString csvError;
    if(!m_conn->query(sql, nullptr, &csvError)) {
        m_messages->setPlainText(QStringLiteral("Import failed: %1").arg(csvError));
    } else {
        const QString info = m_conn->info();
        m_messages->setPlainText(
            QStringLiteral("Imported into `%1`.`%2` — %3")
                .arg(db, target,
                     !info.isEmpty() ? info
                                     : QStringLiteral("%1 row(s)").arg(m_conn->affectedRows())));
        if(m_tableData->loadedTable() == target)
            m_tableData->load(m_conn, db, target);
    }
    m_resultTabs->setCurrentWidget(m_messages);
    refreshBrowser();
}

void ConnectionTab::promptManageForeignKeys(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;

    QList<ForeignKeyDialog::FkDef> fks;
    const auto find = [&](const QString &n) -> ForeignKeyDialog::FkDef * {
        for(auto &f : fks)
            if(f.name == n)
                return &f;
        return nullptr;
    };
    /* canonical shape (qt/db/IDbConnection.h): Name(0) Column(1) Ref_table(2)
     * Ref_column(3) On_update(4) On_delete(5), one row per column */
    for(const QStringList &row : m_conn->listForeignKeys(db, table).rows) {
        const QString name = row.value(0);
        ForeignKeyDialog::FkDef *f = find(name);
        if(!f) {
            ForeignKeyDialog::FkDef nf;
            nf.name = name;
            nf.refTable = row.value(2);
            nf.onDelete =
                row.value(5) == QStringLiteral("NULL") ? QStringLiteral("RESTRICT") : row.value(5);
            nf.onUpdate =
                row.value(4) == QStringLiteral("NULL") ? QStringLiteral("RESTRICT") : row.value(4);
            fks << nf;
            f = &fks.last();
        }
        f->columns << row.value(1);
        f->refColumns << row.value(3);
    }

    QStringList cols, tables;
    for(const QStringList &row : m_conn->listColumns(db, table).rows)
        cols << row.value(0);
    tables = m_conn->listTables(db, QStringLiteral("BASE TABLE"));

    ForeignKeyDialog dlg(db, table, fks, cols, tables, m_conn, this, m_params.driverType);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    const QString limitation = dlg.limitation();
    if(sql.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Foreign Keys"),
                                 limitation.isEmpty() ? QStringLiteral("No changes to apply.")
                                                      : limitation);
        return;
    }
    if(!limitation.isEmpty())
        QMessageBox::warning(this, QStringLiteral("Foreign Keys"), limitation);
    execDdl(sql);
}

void ConnectionTab::promptAlterTable(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;

    /* canonical shape via listColumns(): Field(0) Type(1) Null(2) Key(3)
     * Default(4) Extra(5) Comment(6) — portable across backends, unlike the
     * raw "SHOW FULL COLUMNS" this used to send (MySQL-only syntax, and it
     * additionally broke on every SQLite connection via the same empty-db
     * qualification bug already fixed elsewhere: db is always "" there). */
    const DbResultSet colRs = m_conn->listColumns(db, table);
    if(colRs.rows.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Alter Table"),
                             QStringLiteral("Could not read columns of `%1`.`%2`.").arg(db, table));
        return;
    }
    QList<CreateTableDialog::ColumnDef> cols;
    for(const QStringList &row : colRs.rows) {
        CreateTableDialog::ColumnDef c;
        c.name = row.value(0);
        QString type = row.value(1).trimmed();
        c.isUnsigned = type.contains(QStringLiteral(" unsigned"), Qt::CaseInsensitive);
        type.remove(QStringLiteral(" unsigned"), Qt::CaseInsensitive);
        type.remove(QStringLiteral(" zerofill"), Qt::CaseInsensitive);
        const int lp = type.indexOf('(');
        if(lp >= 0 && type.endsWith(')')) {
            c.length = type.mid(lp + 1, type.size() - lp - 2);
            c.type = type.left(lp).toUpper();
        } else {
            c.type = type.toUpper();
        }
        c.notNull = row.value(2) == QStringLiteral("NO");
        c.pk = row.value(3) == QStringLiteral("PRI");
        c.def = orEmpty(row.value(4));
        c.autoInc = row.value(5).contains(QStringLiteral("auto_increment"), Qt::CaseInsensitive);
        c.comment = orEmpty(row.value(6));
        cols << c;
    }
    if(cols.isEmpty())
        return;

    QString engine, charset;
    if(m_params.driverType == SqlDriverType::Mysql) {
        /* ENGINE/TABLE_COLLATION: MySQL-only information_schema columns —
         * PostgreSQL's information_schema.tables has neither (no storage
         * engines, no per-table charset), so this stays MySQL-only rather
         * than sending a query guaranteed to fail there */
        DbResultSet rs;
        if(m_conn->query(QStringLiteral("SELECT ENGINE, SUBSTRING_INDEX(TABLE_COLLATION,'_',1) "
                                        "FROM information_schema.TABLES "
                                        "WHERE TABLE_SCHEMA='%1' AND TABLE_NAME='%2'")
                             .arg(db, table),
                         &rs, nullptr) &&
           !rs.rows.isEmpty()) {
            engine = orEmpty(rs.rows.first().value(0));
            charset = orEmpty(rs.rows.first().value(1));
        }
    }

    CreateTableDialog dlg(db, table, cols, engine, charset, this, m_params.driverType);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    const QString limitation = dlg.alterLimitation();
    if(sql.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Alter Table"),
                                 limitation.isEmpty() ? QStringLiteral("No changes to apply.")
                                                      : limitation);
        return;
    }
    if(!limitation.isEmpty())
        QMessageBox::warning(this, QStringLiteral("Alter Table"), limitation);
    execDdl(sql);
    if(m_tableData->loadedTable() == table)
        m_tableData->load(m_conn, db, table);
}

void ConnectionTab::promptDumpDatabase(const QString &database)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? defaultDb() : database;
    if(db.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Backup As SQL Dump"),
                                 QStringLiteral("Select a database first."));
        return;
    }
    /* table list for the "which tables" selector */
    const QStringList allTables = m_conn->listTables(db, QStringLiteral("BASE TABLE"));

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Backup `%1` as SQL dump").arg(db));
    auto *structure = new QCheckBox(QStringLiteral("Structure (CREATE TABLE)"), &dlg);
    structure->setChecked(true);
    auto *data = new QCheckBox(QStringLiteral("Data (INSERT statements)"), &dlg);
    data->setChecked(true);
    auto *drops = new QCheckBox(QStringLiteral("Add DROP TABLE before each CREATE"), &dlg);
    drops->setChecked(true);
    auto *routines = new QCheckBox(
        QStringLiteral("Also views / procedures / functions / triggers / events"), &dlg);
    auto *rowsPer = new QSpinBox(&dlg);
    rowsPer->setRange(1, 100000);
    rowsPer->setValue(100);
    rowsPer->setLocale(QLocale::c());

    auto *tableList = new QListWidget(&dlg);
    tableList->setMaximumHeight(180);
    for(const QString &t : allTables) {
        auto *it = new QListWidgetItem(t, tableList);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(Qt::Checked);
    }

    auto *form = new QFormLayout;
    form->addRow(QString(), structure);
    form->addRow(QString(), data);
    form->addRow(QString(), drops);
    form->addRow(QString(), routines);
    form->addRow(QStringLiteral("Rows per INSERT"), rowsPer);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Choose file…"));
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(new QLabel(QStringLiteral("Tables (all, when none checked):"), &dlg));
    lay->addWidget(tableList);
    lay->addWidget(bb);
    if(dlg.exec() != QDialog::Accepted)
        return;

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Backup `%1` as SQL dump").arg(db), db + QStringLiteral(".sql"),
        QStringLiteral("SQL (*.sql);;All (*)"));
    if(path.isEmpty())
        return;

    QStringList picked;
    for(int i = 0; i < tableList->count(); ++i)
        if(tableList->item(i)->checkState() == Qt::Checked)
            picked << tableList->item(i)->text();
    if(picked.size() == tableList->count())
        picked.clear(); /* all → let SqlDump enumerate */

    SqlDump::Options opt;
    opt.structure = structure->isChecked();
    opt.data = data->isChecked();
    opt.addDropTable = drops->isChecked();
    opt.routines = routines->isChecked();
    opt.rowsPerInsert = rowsPer->value();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    const bool ok = dumpDatabaseToFile(db, path, picked, opt, &err);
    QApplication::restoreOverrideCursor();

    m_messages->setPlainText(ok ? QStringLiteral("Dumped `%1` → %2  (%3 KB)")
                                      .arg(db, path)
                                      .arg((QFileInfo(path).size() + 1023) / 1024)
                                : QStringLiteral("Dump failed: %1").arg(err));
    m_resultTabs->setCurrentWidget(m_messages);
}

bool ConnectionTab::dumpDatabaseToFile(const QString &database, const QString &path, QString *error)
{
    return dumpDatabaseToFile(database, path, {}, SqlDump::Options{}, error);
}

bool ConnectionTab::dumpDatabaseToFile(const QString &database, const QString &path,
                                       const QStringList &tables, const SqlDump::Options &opt,
                                       QString *error)
{
    if(!m_conn) {
        if(error)
            *error = QStringLiteral("not connected");
        return false;
    }
    const QString db = database.isEmpty() ? defaultDb() : database;
    QFile f(path);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if(error)
            *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    const bool ok = SqlDump::write(m_conn, db, tables, opt, &f, error);
    f.close();
    return ok;
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
        const QStringList stmts = splitStatements(sql);
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
