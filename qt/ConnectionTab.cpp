#include "ConnectionTab.h"
#include "ObjectBrowser.h"
#include "TableDataView.h"
#include "SqlHighlighter.h"
#include "CreateTableDialog.h"
#include "FindBar.h"
#include "SqlFormat.h"
#include "UserManagerDialog.h"
#include "IndexDialog.h"
#include "ForeignKeyDialog.h"
#include "SqlDump.h"
#include "Icons.h"
#include "wyString.h"

#include <QApplication>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QCheckBox>
#include <QComboBox>
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

#include <mysql/mysql.h>

#include <algorithm>
#include <thread>

namespace {

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
    QTextStream(&f) << QDateTime::currentDateTime().toString(Qt::ISODate)
                    << '\t' << conn << '\t' << one << '\n';
}

/* the last `max` history lines for `conn`, oldest first, as "[time date] sql" */
QStringList loadHistoryFor(const QString &conn, int max)
{
    QFile f(historyPath());
    if(!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QStringList out;
    QTextStream in(&f);
    while(!in.atEnd()) {
        const QStringList p = in.readLine().split('\t');
        if(p.size() < 3 || p[1] != conn)
            continue;
        const QDateTime dt = QDateTime::fromString(p[0], Qt::ISODate);
        out << QStringLiteral("[%1] %2")
                   .arg(dt.isValid() ? dt.toString(QStringLiteral("MMM d  hh:mm:ss"))
                                     : p[0],
                        p.mid(2).join(QLatin1Char('\t')));
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
            return b != QStringLiteral("NULL");   /* NULLs sort to the end */
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
            r0 = qMin(r0, i.row()); r1 = qMax(r1, i.row());
            c0 = qMin(c0, i.column()); c1 = qMax(c1, i.column());
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

/* split on ';', honoring single-quoted strings (good enough for now) */
QStringList splitStatements(const QString &sql)
{
    QStringList out;
    QString cur;
    bool inString = false;
    for(int i = 0; i < sql.size(); ++i) {
        const QChar ch = sql[i];
        if(ch == '\'' ) {
            if(inString && i + 1 < sql.size() && sql[i + 1] == '\'')
                cur += "''", ++i;
            else
                inString = !inString;
        }
        if(ch == ';' && !inString) {
            if(!cur.trimmed().isEmpty())
                out << cur.trimmed();
            cur.clear();
            continue;
        }
        cur += ch;
    }
    if(!cur.trimmed().isEmpty())
        out << cur.trimmed();
    return out;
}

/* runs in a worker thread: dedicated connection per batch, results
 * collected as plain data (no libmariadb objects cross threads) */
QVector<QueryResult> runOnConnection(const ConnectionParams &p,
                                     const QStringList &statements)
{
    QVector<QueryResult> results;
    MYSQL *c = mysql_init(nullptr);
    mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if(!mysql_real_connect(c, p.host.toUtf8(), p.user.toUtf8(),
                           p.password.toUtf8(),
                           p.database.isEmpty() ? nullptr : p.database.toUtf8(),
                           p.port, nullptr, 0)) {
        QueryResult r;
        r.ok = false;
        r.message = QString::fromUtf8(mysql_error(c));
        results.append(r);
        mysql_close(c);
        return results;
    }

    for(const QString &stmt : statements) {
        QueryResult r;
        QElapsedTimer timer;
        timer.start();
        wyString q;
        q.SetAs(stmt.toUtf8().constData());

        if(mysql_query(c, q.GetString()) != 0) {
            r.ok = false;
            r.message = QString::fromUtf8(mysql_error(c));
        } else if(MYSQL_RES *res = mysql_store_result(c)) {
            const unsigned int n = mysql_num_fields(res);
            MYSQL_FIELD *fields = mysql_fetch_fields(res);
            for(unsigned int i = 0; i < n; ++i)
                r.headers << QString::fromUtf8(fields[i].name);
            while(MYSQL_ROW row = mysql_fetch_row(res)) {
                QStringList cells;
                for(unsigned int i = 0; i < n; ++i)
                    cells << (row[i] ? QString::fromUtf8(row[i])
                                     : QStringLiteral("NULL"));
                r.rows << cells;
            }
            mysql_free_result(res);
            r.ok = true;
            r.message = QStringLiteral("%1 row(s)").arg(r.rows.size());
        } else if(mysql_field_count(c) == 0) {
            r.ok = true;
            r.message = QStringLiteral("OK, %1 row(s) affected")
                            .arg((long long)mysql_affected_rows(c));
        } else {
            r.ok = false;
            r.message = QString::fromUtf8(mysql_error(c));
        }
        r.secs = timer.elapsed() / 1000.0;
        results.append(r);
    }

    mysql_close(c);
    return results;
}

} // namespace

ConnectionTab::ConnectionTab(const ConnectionParams &params, QWidget *parent)
    : QWidget(parent), m_params(params)
{
    /* ---- left: object browser ------------------------------------ */
    m_browser = new ObjectBrowser(this);

    /* ---- right-top: editor tabs (Query 1 / History) --------------- */
    m_editor = new CodeEditor(this);
    m_editor->setPlainText(QStringLiteral(
        "SELECT VERSION(), CURRENT_USER();\nSHOW DATABASES;"));
    attachEditor(m_editor, QStringLiteral("Query 1"));

    m_history = new QPlainTextEdit(this);
    m_history->setReadOnly(true);
    m_history->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    /* previous sessions' history for this connection */
    m_historyLines = loadHistoryFor(params.name, 500);
    if(!m_historyLines.isEmpty())
        m_historyLines << QStringLiteral("——— this session ———");

    m_historySearch = new QLineEdit(this);
    m_historySearch->setPlaceholderText(QStringLiteral("filter history…"));
    m_historySearch->setClearButtonEnabled(true);   /* its own ✕ clears the field */
    connect(m_historySearch, &QLineEdit::textChanged, this,
            &ConnectionTab::renderHistory);
    /* explicit label + ellipsis: this wipes the saved log, not the filter box */
    auto *histClear = new QPushButton(QStringLiteral("Clear History…"), this);
    histClear->setToolTip(QStringLiteral("Delete the saved query history file"));
    connect(histClear, &QPushButton::clicked, this, &ConnectionTab::clearHistory);
    auto *histTop = new QHBoxLayout;
    histTop->setContentsMargins(3, 3, 3, 0);
    histTop->addWidget(new QLabel(QStringLiteral("Filter:"), this));
    histTop->addWidget(m_historySearch, 1);
    histTop->addSpacing(8);
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
    m_editorTabs->tabBar()->setExpanding(false);   /* SQLyog left-aligns tabs */
    m_editorTabs->setCornerWidget(
        [&] {
            auto *plus = new QPushButton(QStringLiteral("+"), this);
            plus->setFlat(true);
            plus->setFixedSize(22, 20);
            plus->setStyleSheet(QStringLiteral(
                "QPushButton { background: transparent; color: #3B7DBB; "
                "border: none; font-weight: bold; }"
                "QPushButton:hover { background: #E8F2FA; }"));
            connect(plus, &QPushButton::clicked, this, &ConnectionTab::addEditorTab);
            return plus;
        }(), Qt::TopRightCorner);
    m_editorTabs->addTab(m_editor, Icons::get(QStringLiteral("query_16.ico")),
                         QStringLiteral("Query 1"));
    m_editorTabs->addTab(m_historyPage, Icons::get(QStringLiteral("history.ico")),
                         QStringLiteral("History"));

    /* ---- right-bottom: result tabs -------------------------------- */
    m_messages = new QPlainTextEdit(this);
    m_messages->setReadOnly(true);

    m_info = new QLabel(QStringLiteral("Run a query to see server info."), this);
    m_info->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_info->setWordWrap(true);

    m_resultTabs = new QTabWidget(this);
    m_resultTabs->setObjectName(QStringLiteral("resultTabs"));
    m_resultTabs->setDocumentMode(true);
    m_resultTabs->setTabPosition(QTabWidget::North);
    m_resultTabs->tabBar()->setExpanding(false);   /* SQLyog left-aligns tabs */
    m_tableData = new TableDataView(this);

    m_resultTabs->addTab(m_messages,   QStringLiteral("1 Messages"));
    m_resultTabs->addTab(m_tableData,  QStringLiteral("2 Table Data"));
    m_resultTabs->addTab(m_info,       QStringLiteral("3 Info"));
    m_resultTabs->setCurrentIndex(0);

    /* nag-bar replacement — solid blue strip above the editor (Flat theme) */
    m_infoBar = new QLabel(this);
    m_infoBar->setObjectName(QStringLiteral("infoStrip"));

    m_findBar = new FindBar([this] { return currentEditor(); }, this);

    auto *editorSide = new QWidget(this);
    auto *editorCol = new QVBoxLayout(editorSide);
    editorCol->setContentsMargins(0, 0, 0, 0);
    editorCol->setSpacing(0);
    editorCol->addWidget(m_infoBar);
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
    mainSplit->setSizes({215, 985});   /* spec: object browser ~1/5 width */

    /* bottom LIMIT strip — solid blue, "All" combo hard left (Flat theme) */
    m_limitCombo = new QComboBox(this);
    m_limitCombo->addItems({ QStringLiteral("All"), QStringLiteral("1000"),
                             QStringLiteral("5000"), QStringLiteral("10000") });
    auto *limitStrip = new QFrame(this);
    limitStrip->setObjectName(QStringLiteral("limitStrip"));
    auto *limitRow = new QHBoxLayout(limitStrip);
    limitRow->setContentsMargins(4, 2, 4, 2);
    limitRow->addWidget(m_limitCombo);
    limitRow->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(mainSplit, 1);
    layout->addWidget(limitStrip);

    /* ---- open the connection ------------------------------------- */
    m_conn = mysql_init(nullptr);
    mysql_options(m_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    { unsigned int on = 1; mysql_options(m_conn, MYSQL_OPT_LOCAL_INFILE, &on); }
    if(!mysql_real_connect(m_conn, m_params.host.toUtf8(), m_params.user.toUtf8(),
                           m_params.password.toUtf8(),
                           m_params.database.isEmpty() ? nullptr
                                                       : m_params.database.toUtf8(),
                           m_params.port, nullptr, 0)) {
        m_messages->setPlainText(QStringLiteral("Connection failed: ")
                                 + mysql_error(m_conn));
        mysql_close(m_conn);
        m_conn = nullptr;
        return;
    }

    connect(m_browser, &ObjectBrowser::tableActivated, this,
            [this](const QString &db, const QString &table) {
        if(m_conn) {
            m_tableData->load(m_conn, db, table);
            m_resultTabs->setCurrentWidget(m_tableData);
        }
    });
    connect(m_tableData, &TableDataView::statusMessage, this,
            [this](const QString &text) {
        m_messages->appendPlainText(text);
    });

    connect(m_browser, &ObjectBrowser::dropTableRequested, this,
            &ConnectionTab::dropTable);
    connect(m_browser, &ObjectBrowser::createTableRequested, this,
            [this](const QString &db) { promptCreateTable(db); });
    connect(m_browser, &ObjectBrowser::alterTableRequested, this,
            [this](const QString &db, const QString &table) {
        promptAlterTable(db, table);
    });
    connect(m_browser, &ObjectBrowser::renameTableRequested, this,
            &ConnectionTab::promptRenameTable);
    connect(m_browser, &ObjectBrowser::copyTableRequested, this,
            &ConnectionTab::promptCopyTable);
    connect(m_browser, &ObjectBrowser::manageIndexesRequested, this,
            &ConnectionTab::promptManageIndexes);
    connect(m_browser, &ObjectBrowser::manageForeignKeysRequested, this,
            &ConnectionTab::promptManageForeignKeys);
    connect(m_browser, &ObjectBrowser::dumpDatabaseRequested, this,
            [this](const QString &db) { promptDumpDatabase(db); });
    connect(m_browser, &ObjectBrowser::copyDatabaseRequested, this,
            [this](const QString &db) { promptCopyDatabase(db); });
    connect(m_browser, &ObjectBrowser::importCsvRequested, this,
            &ConnectionTab::promptImportCsv);
    connect(m_browser, &ObjectBrowser::importXmlRequested, this,
            &ConnectionTab::promptImportXml);
    connect(m_browser, &ObjectBrowser::truncateTableRequested, this,
            &ConnectionTab::truncateTable);

    m_infoBar->setText(QStringLiteral(
        "OpenYog — connected to %1@%2:%3%4")
        .arg(m_params.user, m_params.host).arg(m_params.port)
        .arg(m_params.database.isEmpty() ? QString()
                                         : QStringLiteral("/") + m_params.database));

    m_browser->setConnectionLabel(
        QStringLiteral("%1@%2").arg(m_params.user, m_params.host));
    m_browser->loadDatabases(m_conn, m_params.database);
    updateCompletions();
    m_messages->setPlainText(QStringLiteral(
        "Connected to %1:%2 as %3\nServer version: %4")
        .arg(m_params.host).arg(m_params.port)
        .arg(m_params.user, QString::fromUtf8(mysql_get_server_info(m_conn))));

    QStringList dbs;
    if(mysql_query(m_conn, "SHOW DATABASES") == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res))
                if(row[0])
                    dbs << QString::fromUtf8(row[0]);
            mysql_free_result(res);
        }
    }
    m_databases = dbs;
    emit databasesChanged(dbs, m_params.database);
}

ConnectionTab::~ConnectionTab()
{
    if(m_conn)
        mysql_close(m_conn);
}

CodeEditor *ConnectionTab::currentEditor() const
{
    if(auto *ed = qobject_cast<CodeEditor *>(m_editorTabs->currentWidget()))
        return ed;
    return m_editor;
}

void ConnectionTab::attachEditor(CodeEditor *ed, const QString &title)
{
    new SqlHighlighter(ed->document());
    connect(ed, &QPlainTextEdit::cursorPositionChanged, this, [this, ed] {
        const QTextCursor c = ed->textCursor();
        emit cursorMoved(QStringLiteral("Ln %1, Col %2")
                             .arg(c.blockNumber() + 1).arg(c.positionInBlock() + 1));
    });
    ed->setCompletions(m_completions);
    Q_UNUSED(title);
}

/* schema identifiers (tables + columns of the current db) for autocomplete */
void ConnectionTab::updateCompletions()
{
    m_completions.clear();
    if(m_conn && !m_params.database.isEmpty()) {
        wyString q;
        q.Sprintf("SELECT TABLE_NAME, COLUMN_NAME FROM information_schema.COLUMNS "
                  "WHERE TABLE_SCHEMA = '%s'",
                  QString(m_params.database).replace('\'', QStringLiteral("''"))
                      .toUtf8().constData());
        if(mysql_query(m_conn, q.GetString()) == 0) {
            if(MYSQL_RES *res = mysql_store_result(m_conn)) {
                QSet<QString> seen;
                while(MYSQL_ROW row = mysql_fetch_row(res)) {
                    if(row[0]) seen.insert(QString::fromUtf8(row[0]));
                    if(row[1]) seen.insert(QString::fromUtf8(row[1]));
                }
                mysql_free_result(res);
                m_completions = QStringList(seen.cbegin(), seen.cend());
            }
        }
    }
    for(int i = 0; i < m_editorTabs->count(); ++i)
        if(auto *ed = qobject_cast<CodeEditor *>(m_editorTabs->widget(i)))
            ed->setCompletions(m_completions);
}

void ConnectionTab::addEditorTab()
{
    int maxN = 0;
    for(int i = 0; i < m_editorTabs->count(); ++i) {
        const QString t = m_editorTabs->tabText(i);
        if(t.startsWith(QStringLiteral("Query ")))
            maxN = qMax(maxN, t.mid(6).toInt());
    }
    auto *ed = new CodeEditor(this);
    attachEditor(ed, {});
    const QString title = QStringLiteral("Query %1").arg(maxN + 1);
    const int histIdx = m_editorTabs->indexOf(m_history);
    m_editorTabs->insertTab(histIdx == -1 ? m_editorTabs->count() : histIdx,
                            ed, Icons::get(QStringLiteral("query_16.ico")), title);
    m_editorTabs->setCurrentWidget(ed);
}

void ConnectionTab::logHistory(const QString &sql)
{
    m_historyLines << QStringLiteral("[%1] %2")
        .arg(QTime::currentTime().toString(QStringLiteral("hh:mm:ss")), sql);
    renderHistory();
    appendHistoryLine(m_params.name, sql);
}

void ConnectionTab::renderHistory()
{
    const QString filter = m_historySearch->text().trimmed();
    QStringList shown;
    for(const QString &l : std::as_const(m_historyLines))
        if(filter.isEmpty() || l.contains(filter, Qt::CaseInsensitive))
            shown << l;
    m_history->setPlainText(shown.join(QLatin1Char('\n')));
    m_history->moveCursor(QTextCursor::End);
}

void ConnectionTab::clearHistory()
{
    if(QMessageBox::question(this, QStringLiteral("Clear History"),
           QStringLiteral("Delete all saved query history for every connection?"))
           != QMessageBox::Yes)
        return;
    QFile(historyPath()).resize(0);
    m_historyLines.clear();
    renderHistory();
}

void ConnectionTab::runQuery()
{
    runStatements(splitStatements(currentEditor()->toPlainText()),
                  QStringLiteral("Result"));
}

void ConnectionTab::runStatements(const QStringList &statements,
                                  const QString &tabPrefix)
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
    m_messages->setPlainText(QStringLiteral("Executing %1 statement(s)…")
                                 .arg(statements.size()));
    m_resultTabs->setCurrentWidget(m_messages);

    /* worker thread: fresh connection, plain-data results */
    QPointer<ConnectionTab> guard(this);
    const ConnectionParams p = m_params;
    std::thread([guard, p, statements, tabPrefix] {
        const QVector<QueryResult> results = runOnConnection(p, statements);
        QMetaObject::invokeMethod(guard, [guard, results, tabPrefix] {
            if(guard)
                guard->applyResults(results, tabPrefix);
        }, Qt::QueuedConnection);
    }).detach();
}

void ConnectionTab::openTable(const QString &db, const QString &table)
{
    if(m_running)
        return;
    const QString sql = QStringLiteral("SELECT * FROM `%1`.`%2` LIMIT 1000")
                            .arg(db, table);
    logHistory(sql);
    runStatements(QStringList{ sql },
                  QStringLiteral("%1.%2").arg(db, table));
}

void ConnectionTab::applyResults(const QVector<QueryResult> &results,
                                 const QString &tabPrefix)
{
    m_running = false;

    /* drop the previous batch's result grids */
    for(QWidget *w : std::as_const(m_dynamicResultTabs)) {
        const int idx = m_resultTabs->indexOf(w);
        if(idx >= 0)
            m_resultTabs->removeTab(idx);
        delete w;
    }
    m_dynamicResultTabs.clear();

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
                           .arg(i + 1).arg(r.message)
                           .arg(r.secs, 0, 'f', 2);
            continue;
        }
        summary += QStringLiteral("✓ [%1] %2 (%3 sec)\n")
                       .arg(i + 1).arg(r.message).arg(r.secs, 0, 'f', 2);

        if(!r.headers.isEmpty()) {
            ++grids;
            addResultGrid(r, QStringLiteral("%1 %2")
                                    .arg(tabPrefix).arg(grids));
            if(!firstGrid)
                firstGrid = m_dynamicResultTabs.first();
        }
    }

    if(grids)
        summary += QStringLiteral("\n%1 result set(s) displayed.").arg(grids);
    m_messages->setPlainText(summary);
    m_resultTabs->setCurrentWidget(
        firstGrid ? firstGrid : static_cast<QWidget *>(m_messages));

    const double exec = total;
    emit executed(QStringLiteral("Exec: %1 sec | Total: %2 sec")
                      .arg(exec, 0, 'f', 2).arg(m_totalSecs, 0, 'f', 2));
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

    m_resultTabs->addTab(grid, Icons::get(QStringLiteral("grid_view.ico")), title);
    m_dynamicResultTabs.append(grid);
    m_lastGrid = grid;
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
    const QString f = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save SQL"), QStringLiteral("query.sql"),
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
    m_editorTabs->setCurrentWidget(m_historyPage);
}

void ConnectionTab::exportResult()
{
    QAbstractItemModel *m = m_lastGrid ? m_lastGrid->model() : nullptr;
    if(!m || m->rowCount() == 0) {
        m_messages->appendPlainText(QStringLiteral("no result set to export"));
        return;
    }
    QString filter;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export result as…"), QStringLiteral("result.csv"),
        QStringLiteral("CSV (*.csv);;HTML (*.html *.htm);;JSON (*.json);;"
                       "Markdown (*.md)"),
        &filter);
    if(path.isEmpty())
        return;
    QFile file(path);
    if(!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_messages->appendPlainText(QStringLiteral("cannot write ") + path);
        return;
    }

    const int cols = m->columnCount(), rows = m->rowCount();
    QStringList headers;
    for(int c = 0; c < cols; ++c)
        headers << m->headerData(c, Qt::Horizontal).toString();
    const auto cell = [&](int r, int c) { return m->index(r, c).data().toString(); };
    const QString fmt = filter.startsWith(QStringLiteral("HTML"))     ? QStringLiteral("html")
                      : filter.startsWith(QStringLiteral("JSON"))     ? QStringLiteral("json")
                      : filter.startsWith(QStringLiteral("Markdown")) ? QStringLiteral("md")
                      : path.endsWith(QStringLiteral(".html")) || path.endsWith(QStringLiteral(".htm"))
                                                                     ? QStringLiteral("html")
                      : path.endsWith(QStringLiteral(".json"))        ? QStringLiteral("json")
                      : path.endsWith(QStringLiteral(".md"))          ? QStringLiteral("md")
                                                                     : QStringLiteral("csv");
    QTextStream out(&file);

    if(fmt == QStringLiteral("json")) {
        QJsonArray arr;
        for(int r = 0; r < rows; ++r) {
            QJsonObject o;
            for(int c = 0; c < cols; ++c) {
                const QString v = cell(r, c);
                o.insert(headers[c], v == QStringLiteral("NULL")
                                         ? QJsonValue(QJsonValue::Null)
                                         : QJsonValue(v));
            }
            arr.append(o);
        }
        out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    } else if(fmt == QStringLiteral("html")) {
        const auto esc = [](QString s) {
            return s.replace('&', QStringLiteral("&amp;"))
                    .replace('<', QStringLiteral("&lt;"))
                    .replace('>', QStringLiteral("&gt;"));
        };
        out << "<!doctype html><meta charset=\"utf-8\">\n"
               "<style>table{border-collapse:collapse;font:13px sans-serif}"
               "th,td{border:1px solid #ccc;padding:3px 7px}"
               "th{background:#3B7DBB;color:#fff}</style>\n<table>\n<tr>";
        for(const QString &h : std::as_const(headers))
            out << "<th>" << esc(h) << "</th>";
        out << "</tr>\n";
        for(int r = 0; r < rows; ++r) {
            out << "<tr>";
            for(int c = 0; c < cols; ++c)
                out << "<td>" << esc(cell(r, c)) << "</td>";
            out << "</tr>\n";
        }
        out << "</table>\n";
    } else if(fmt == QStringLiteral("md")) {
        const auto esc = [](QString s) { return s.replace('|', QStringLiteral("\\|")); };
        out << "| " << [&] { QStringList h; for(const QString &x : std::as_const(headers)) h << esc(x); return h.join(QStringLiteral(" | ")); }() << " |\n";
        out << "|" << QString(QStringLiteral(" --- |")).repeated(cols) << "\n";
        for(int r = 0; r < rows; ++r) {
            QStringList row;
            for(int c = 0; c < cols; ++c)
                row << esc(cell(r, c));
            out << "| " << row.join(QStringLiteral(" | ")) << " |\n";
        }
    } else {   /* csv */
        const auto q = [](QString s) {
            if(s.contains('"') || s.contains(',') || s.contains('\n'))
                return '"' + s.replace('"', QStringLiteral("\"\"")) + '"';
            return s;
        };
        out << [&] { QStringList h; for(const QString &x : std::as_const(headers)) h << q(x); return h.join(QLatin1Char(',')); }() << "\r\n";
        for(int r = 0; r < rows; ++r) {
            QStringList row;
            for(int c = 0; c < cols; ++c)
                row << q(cell(r, c));
            out << row.join(QLatin1Char(',')) << "\r\n";
        }
    }
    m_messages->appendPlainText(
        QStringLiteral("Exported %1 row(s) as %2 → %3")
            .arg(rows).arg(fmt.toUpper(), path));
}

void ConnectionTab::refreshBrowser()
{
    if(m_conn) {
        m_browser->loadDatabases(m_conn, m_params.database);
        updateCompletions();
    }
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
    const QString from = QInputDialog::getText(
        this, QStringLiteral("Replace"), QStringLiteral("Find what:"),
        QLineEdit::Normal, m_lastFind, &ok);
    if(!ok || from.isEmpty())
        return;
    const QString to = QInputDialog::getText(
        this, QStringLiteral("Replace"), QStringLiteral("Replace with:"),
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
    QTextCursor c = ed->textCursor();
    c.select(QTextCursor::Document);
    c.insertText(text);
    emit executed(QStringLiteral("Replaced %1 occurrence(s)").arg(n));
}

void ConnectionTab::promptGoto()
{
    auto *ed = currentEditor();
    if(!ed)
        return;
    bool ok = false;
    const int line = QInputDialog::getInt(
        this, QStringLiteral("Go To Line"), QStringLiteral("Line number:"),
        ed->textCursor().blockNumber() + 1, 1,
        ed->document()->blockCount(), 1, &ok);
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
    QTextCursor c = ed->textCursor();

    if(scope == 2) {                                   /* whole editor */
        const QString f = SqlFormat::pretty(ed->toPlainText());
        c.select(QTextCursor::Document);
        c.insertText(f);
        return;
    }
    if(scope == 1 && c.hasSelection()) {              /* selection */
        c.insertText(SqlFormat::pretty(c.selectedText()
                                          .replace(QChar::ParagraphSeparator, '\n')));
        return;
    }
    /* current statement: expand to the surrounding ';' boundaries */
    const QString all = ed->toPlainText();
    int pos = c.position();
    int start = all.lastIndexOf(';', qMax(0, pos - 1)) + 1;
    int end = all.indexOf(';', pos);
    if(end < 0)
        end = all.size();
    else
        ++end;                                        /* include the ';' */
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    c.insertText(SqlFormat::pretty(all.mid(start, end - start)));
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

void ConnectionTab::openTableData(const QString &db, const QString &table)
{
    if(!m_conn)
        return;
    m_tableData->load(m_conn, db, table);
    m_resultTabs->setCurrentWidget(m_tableData);
}

void ConnectionTab::promptCreateTable(const QString &database)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? m_params.database : database;
    CreateTableDialog dlg(db, this);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    if(sql.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Create Table"),
            QStringLiteral("Nothing to create — a table name and at least "
                           "one column are required."));
        return;
    }
    execDdl(sql);   /* execDdl already refreshes the object browser on success */
}

void ConnectionTab::dropTable(const QString &database, const QString &table)
{
    const QString db = database.isEmpty() ? m_params.database : database;
    if(table.isEmpty() || QMessageBox::question(this,
            QStringLiteral("Drop Table"),
            QStringLiteral("Permanently DROP table `%1`.`%2`?").arg(db, table))
            != QMessageBox::Yes)
        return;
    execDdl(QStringLiteral("DROP TABLE `%1`.`%2`").arg(db, table));
    if(m_tableData->loadedTable() == table)
        m_tableData->clear();
}

void ConnectionTab::truncateTable(const QString &database, const QString &table)
{
    const QString db = database.isEmpty() ? m_params.database : database;
    if(table.isEmpty() || QMessageBox::question(this,
            QStringLiteral("Truncate Table"),
            QStringLiteral("Delete ALL rows of `%1`.`%2`?").arg(db, table))
            != QMessageBox::Yes)
        return;
    execDdl(QStringLiteral("TRUNCATE TABLE `%1`.`%2`").arg(db, table));
    if(m_tableData->loadedTable() == table)
        m_tableData->load(m_conn, db, table);   /* empty grid */
}

void ConnectionTab::promptRenameTable(const QString &database,
                                      const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? m_params.database : database;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename Table"),
        QStringLiteral("New name for `%1`:").arg(table),
        QLineEdit::Normal, table, &ok);
    if(!ok || name.trimmed().isEmpty() || name == table)
        return;
    execDdl(QStringLiteral("RENAME TABLE `%1`.`%2` TO `%1`.`%3`")
                .arg(db, table, name.trimmed()));
    if(m_tableData->loadedTable() == table)
        m_tableData->load(m_conn, db, name.trimmed());
}

void ConnectionTab::promptCopyTable(const QString &database, const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString srcDb = database.isEmpty() ? m_params.database : database;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Duplicate Table `%1`").arg(table));
    auto *name = new QLineEdit(table + QStringLiteral("_copy"), &dlg);
    auto *targetDb = new QComboBox(&dlg);
    targetDb->addItems(m_databases.isEmpty() ? QStringList{ srcDb } : m_databases);
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
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
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
    const QString src = QStringLiteral("`%1`.`%2`").arg(srcDb, table);
    const QString dst = QStringLiteral("`%1`.`%2`").arg(tgtDb, tgt);

    if(wantStructure->isChecked()) {
        if(!execDdl(QStringLiteral("CREATE TABLE %1 LIKE %2").arg(dst, src)))
            return;
    }
    if(wantData->isChecked())
        execDdl(QStringLiteral("INSERT INTO %1 SELECT * FROM %2").arg(dst, src));
}

void ConnectionTab::promptManageIndexes(const QString &database,
                                        const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? m_params.database : database;

    QList<IndexDialog::IndexDef> indexes;
    const auto findIx = [&](const QString &n) -> IndexDialog::IndexDef * {
        for(auto &ix : indexes)
            if(ix.name == n)
                return &ix;
        return nullptr;
    };
    wyString q;
    q.Sprintf("SHOW INDEX FROM `%s`.`%s`", db.toUtf8().constData(),
              table.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) != 0) {
        QMessageBox::warning(this, QStringLiteral("Manage Indexes"),
                             QString::fromUtf8(mysql_error(m_conn)));
        return;
    }
    if(MYSQL_RES *res = mysql_store_result(m_conn)) {
        while(MYSQL_ROW row = mysql_fetch_row(res)) {
            /* 1=Non_unique 2=Key_name 4=Column_name */
            const QString name = QString::fromUtf8(row[2] ? row[2] : "");
            const QString col  = QString::fromUtf8(row[4] ? row[4] : "");
            IndexDialog::IndexDef *ix = findIx(name);
            if(!ix) {
                IndexDialog::IndexDef nd;
                nd.name = name;
                nd.unique = row[1] && QString::fromUtf8(row[1]) == QStringLiteral("0");
                nd.primary = name == QStringLiteral("PRIMARY");
                indexes << nd;
                ix = &indexes.last();
            }
            ix->columns << col;
        }
        mysql_free_result(res);
    }

    QStringList cols;
    q.Sprintf("SHOW COLUMNS FROM `%s`.`%s`", db.toUtf8().constData(),
              table.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res))
                if(row[0])
                    cols << QString::fromUtf8(row[0]);
            mysql_free_result(res);
        }
    }

    IndexDialog dlg(db, table, indexes, cols, this);
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
    const QString srcDb = database.isEmpty() ? m_params.database : database;
    if(srcDb.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Copy Database"),
            QStringLiteral("Select a database first."));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Copy Database `%1`").arg(srcDb));
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
    auto *dropFirst = new QCheckBox(
        QStringLiteral("Drop target database first if it exists"), &dlg);
    auto *wantRoutines = new QCheckBox(
        QStringLiteral("Also copy views, routines, triggers, events"), &dlg);
    wantRoutines->setChecked(true);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Target host"), tHost);
    form->addRow(QStringLiteral("Target port"), tPort);
    form->addRow(QStringLiteral("Target user"), tUser);
    form->addRow(QStringLiteral("Target password"), tPass);
    form->addRow(QStringLiteral("New database name"), name);
    form->addRow(QString(), wantData);
    form->addRow(QString(), wantRoutines);
    form->addRow(QString(), dropFirst);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Copy"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(new QLabel(QStringLiteral(
        "Same host + port + user → fast CREATE … LIKE copy; a different target "
        "streams a dump over a fresh connection. DEFINER clauses are stripped."),
        &dlg));
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return;

    const QString tgt = name->text().trimmed();
    if(tgt.isEmpty())
        return;
    const bool sameServer = tHost->text().trimmed() == m_params.host
                            && tPort->value() == m_params.port
                            && tUser->text().trimmed() == m_params.user;
    if(sameServer && tgt == srcDb) {
        QMessageBox::information(this, QStringLiteral("Copy Database"),
            QStringLiteral("Target must differ from the source on the same server."));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    bool ok;
    if(sameServer) {
        ok = copyDatabaseTo(srcDb, tgt, wantData->isChecked(),
                            dropFirst->isChecked(), wantRoutines->isChecked(), &err);
    } else {
        MYSQL *dst = mysql_init(nullptr);
        mysql_options(dst, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        if(!mysql_real_connect(dst, tHost->text().trimmed().toUtf8(),
                               tUser->text().trimmed().toUtf8(),
                               tPass->text().toUtf8(), nullptr,
                               tPort->value(), nullptr, 0)) {
            err = QStringLiteral("target connect failed: %1")
                      .arg(QString::fromUtf8(mysql_error(dst)));
            mysql_close(dst);
            ok = false;
        } else {
            const QString tq = QString(tgt).replace('`', QStringLiteral("``"));
            if(dropFirst->isChecked())
                mysql_query(dst, QStringLiteral("DROP DATABASE IF EXISTS `%1`")
                                     .arg(tq).toUtf8().constData());
            mysql_query(dst, QStringLiteral("CREATE DATABASE IF NOT EXISTS `%1` "
                                            "CHARACTER SET utf8mb4")
                                 .arg(tq).toUtf8().constData());
            mysql_query(dst, QStringLiteral("USE `%1`").arg(tq).toUtf8().constData());
            SqlDump::Options opt;
            opt.data = wantData->isChecked();
            opt.routines = wantRoutines->isChecked();
            ok = SqlDump::forEachStatement(
                m_conn, srcDb, {}, opt,
                [&](const QString &stmt) {
                    if(mysql_query(dst, stmt.toUtf8().constData()) == 0)
                        return true;
                    err = QStringLiteral("%1\n  at: %2")
                              .arg(QString::fromUtf8(mysql_error(dst)),
                                   stmt.left(120));
                    return false;
                },
                err.isEmpty() ? &err : nullptr);
            mysql_close(dst);
        }
    }
    QApplication::restoreOverrideCursor();

    m_messages->setPlainText(ok
        ? QStringLiteral("Copied `%1` → %2`%3`.")
              .arg(srcDb,
                   sameServer ? QString()
                              : QStringLiteral("%1:%2/").arg(tHost->text().trimmed())
                                    .arg(tPort->value()),
                   tgt)
        : QStringLiteral("Copy failed:\n%1").arg(err));
    m_resultTabs->setCurrentWidget(m_messages);
    refreshBrowser();
}

bool ConnectionTab::copyDatabaseTo(const QString &srcDb, const QString &tgtDb,
                                   bool withData, bool dropFirst,
                                   bool withRoutines, QString *error)
{
    if(!m_conn || srcDb.isEmpty() || tgtDb.isEmpty() || srcDb == tgtDb) {
        if(error) *error = QStringLiteral("bad source/target");
        return false;
    }
    const QString sb = QString(srcDb).replace('`', QStringLiteral("``"));

    /* one-row helper: run `sql`, return column `col` of the first row */
    const auto oneRow = [&](const QString &sql, int col) -> QString {
        QString out;
        if(mysql_query(m_conn, sql.toUtf8().constData()) == 0) {
            if(MYSQL_RES *r = mysql_store_result(m_conn)) {
                if(MYSQL_ROW row = mysql_fetch_row(r))
                    out = QString::fromUtf8(row[col] ? row[col] : "");
                mysql_free_result(r);
            }
        }
        return out;
    };
    /* names from a single-column query */
    const auto nameList = [&](const QString &sql, int col) {
        QStringList out;
        if(mysql_query(m_conn, sql.toUtf8().constData()) == 0) {
            if(MYSQL_RES *r = mysql_store_result(m_conn)) {
                while(MYSQL_ROW row = mysql_fetch_row(r))
                    if(row[col]) out << QString::fromUtf8(row[col]);
                mysql_free_result(r);
            }
        }
        return out;
    };
    static const QRegularExpression kDefiner(
        QStringLiteral("DEFINER=`[^`]*`@`[^`]*` "));
    const auto retarget = [&](QString ddl) {
        return ddl.remove(kDefiner)
                  .replace(QStringLiteral("`%1`.").arg(srcDb),
                           QStringLiteral("`%1`.").arg(tgtDb));
    };

    const auto bq = [](QString s) { return s.replace('`', QStringLiteral("``")); };
    const QString tb = bq(tgtDb);

    /* base tables — a failing SHOW here (e.g. no such source db) must abort,
     * not silently "succeed" with an empty target */
    QStringList tables;
    {
        const QByteArray q =
            QStringLiteral("SHOW FULL TABLES FROM `%1` WHERE Table_type='BASE TABLE'")
                .arg(sb).toUtf8();
        if(mysql_query(m_conn, q.constData()) != 0) {
            if(error)
                *error = QString::fromUtf8(mysql_error(m_conn));
            return false;
        }
        if(MYSQL_RES *r = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(r))
                if(row[0])
                    tables << QString::fromUtf8(row[0]);
            mysql_free_result(r);
        }
    }

    QStringList stmts;
    if(dropFirst)
        stmts << QStringLiteral("DROP DATABASE IF EXISTS `%1`").arg(tb);
    stmts << QStringLiteral("CREATE DATABASE IF NOT EXISTS `%1`").arg(tb);
    stmts << QStringLiteral("SET FOREIGN_KEY_CHECKS=0");
    for(const QString &t : std::as_const(tables)) {
        const QString tq = bq(t);
        stmts << QStringLiteral("CREATE TABLE `%1`.`%2` LIKE `%3`.`%2`")
                     .arg(tb, tq, sb);
        if(withData)
            stmts << QStringLiteral("INSERT INTO `%1`.`%2` SELECT * FROM `%3`.`%2`")
                         .arg(tb, tq, sb);
    }

    /* CREATE TABLE … LIKE does not carry FK constraints (MariaDB) — copy them
     * explicitly from information_schema once every table exists (FK checks
     * are off, so create order is irrelevant) */
    {
        struct Fk { QString name, tbl, refTbl, onDel, onUpd; QStringList cols, refCols; };
        QList<Fk> fks;
        wyString fq;
        fq.Sprintf(
            "SELECT k.CONSTRAINT_NAME, k.TABLE_NAME, k.COLUMN_NAME, "
            "k.REFERENCED_TABLE_NAME, k.REFERENCED_COLUMN_NAME, "
            "r.DELETE_RULE, r.UPDATE_RULE "
            "FROM information_schema.KEY_COLUMN_USAGE k "
            "JOIN information_schema.REFERENTIAL_CONSTRAINTS r "
            "  ON r.CONSTRAINT_SCHEMA=k.CONSTRAINT_SCHEMA "
            "  AND r.CONSTRAINT_NAME=k.CONSTRAINT_NAME "
            "WHERE k.TABLE_SCHEMA='%s' AND k.REFERENCED_TABLE_NAME IS NOT NULL "
            "ORDER BY k.CONSTRAINT_NAME, k.ORDINAL_POSITION",
            sb.toUtf8().constData());
        if(mysql_query(m_conn, fq.GetString()) == 0) {
            if(MYSQL_RES *r = mysql_store_result(m_conn)) {
                while(MYSQL_ROW row = mysql_fetch_row(r)) {
                    const QString name = QString::fromUtf8(row[0] ? row[0] : "");
                    Fk *f = nullptr;
                    for(auto &e : fks)
                        if(e.name == name && e.tbl == QString::fromUtf8(row[1] ? row[1] : "")) {
                            f = &e; break;
                        }
                    if(!f) {
                        fks << Fk{ name, QString::fromUtf8(row[1] ? row[1] : ""),
                                   QString::fromUtf8(row[3] ? row[3] : ""),
                                   QString::fromUtf8(row[5] ? row[5] : "RESTRICT"),
                                   QString::fromUtf8(row[6] ? row[6] : "RESTRICT"), {}, {} };
                        f = &fks.last();
                    }
                    f->cols    << QString::fromUtf8(row[2] ? row[2] : "");
                    f->refCols << QString::fromUtf8(row[4] ? row[4] : "");
                }
                mysql_free_result(r);
            }
        }
        for(const Fk &f : std::as_const(fks)) {
            const auto btlist = [&](const QStringList &l) {
                QStringList o;
                for(const QString &c : l) o << QStringLiteral("`%1`").arg(bq(c));
                return o.join(QStringLiteral(", "));
            };
            stmts << QStringLiteral(
                "ALTER TABLE `%1`.`%2` ADD CONSTRAINT `%3` FOREIGN KEY (%4) "
                "REFERENCES `%1`.`%5` (%6) ON DELETE %7 ON UPDATE %8")
                .arg(tb, bq(f.tbl), bq(f.name), btlist(f.cols),
                     bq(f.refTbl), btlist(f.refCols), f.onDel, f.onUpd);
        }
    }

    if(withRoutines) {
        stmts << QStringLiteral("USE `%1`").arg(tb);

        for(const QString &v : nameList(
                QStringLiteral("SHOW FULL TABLES FROM `%1` WHERE Table_type='VIEW'")
                    .arg(sb), 0)) {
            QString ddl = retarget(oneRow(
                QStringLiteral("SHOW CREATE VIEW `%1`.`%2`").arg(srcDb, v), 1));
            ddl.replace(QStringLiteral(" VIEW `%1` ").arg(v),
                        QStringLiteral(" VIEW `%1`.`%2` ").arg(tgtDb, v));
            if(!ddl.isEmpty())
                stmts << ddl;
        }

        /* procedures + functions */
        struct R { QString name, type; };
        QList<R> routines;
        if(mysql_query(m_conn,
               QStringLiteral("SELECT ROUTINE_NAME, ROUTINE_TYPE FROM "
                              "information_schema.ROUTINES WHERE ROUTINE_SCHEMA='%1'")
                   .arg(sb).toUtf8().constData()) == 0) {
            if(MYSQL_RES *r = mysql_store_result(m_conn)) {
                while(MYSQL_ROW row = mysql_fetch_row(r))
                    if(row[0] && row[1])
                        routines << R{ QString::fromUtf8(row[0]),
                                       QString::fromUtf8(row[1]) };
                mysql_free_result(r);
            }
        }
        for(const R &rt : std::as_const(routines)) {
            const bool proc = rt.type == QStringLiteral("PROCEDURE");
            QString ddl = retarget(oneRow(
                QStringLiteral("SHOW CREATE %1 `%2`.`%3`")
                    .arg(proc ? QStringLiteral("PROCEDURE")
                              : QStringLiteral("FUNCTION"), srcDb, rt.name), 2));
            ddl.replace(QStringLiteral("%1 `%2`")
                            .arg(proc ? QStringLiteral("PROCEDURE")
                                      : QStringLiteral("FUNCTION"), rt.name),
                        QStringLiteral("%1 `%2`.`%3`")
                            .arg(proc ? QStringLiteral("PROCEDURE")
                                      : QStringLiteral("FUNCTION"), tgtDb, rt.name));
            if(!ddl.isEmpty())
                stmts << ddl;
        }

        /* triggers: SHOW TRIGGERS = Trigger,Event,Table,Statement,Timing,… */
        if(mysql_query(m_conn,
               QStringLiteral("SHOW TRIGGERS FROM `%1`").arg(sb)
                   .toUtf8().constData()) == 0) {
            if(MYSQL_RES *r = mysql_store_result(m_conn)) {
                while(MYSQL_ROW row = mysql_fetch_row(r)) {
                    if(!row[0])
                        continue;
                    stmts << QStringLiteral(
                        "CREATE TRIGGER `%1`.`%2` %3 %4 ON `%1`.`%5` "
                        "FOR EACH ROW %6")
                        .arg(tgtDb, QString::fromUtf8(row[0]),
                             QString::fromUtf8(row[4] ? row[4] : ""),
                             QString::fromUtf8(row[1] ? row[1] : ""),
                             QString::fromUtf8(row[2] ? row[2] : ""),
                             QString::fromUtf8(row[3] ? row[3] : ""));
                }
                mysql_free_result(r);
            }
        }

        for(const QString &e : nameList(
                QStringLiteral("SHOW EVENTS FROM `%1`").arg(sb), 1)) {
            QString ddl = retarget(oneRow(
                QStringLiteral("SHOW CREATE EVENT `%1`.`%2`").arg(srcDb, e), 3));
            ddl.replace(QStringLiteral(" EVENT `%1` ").arg(e),
                        QStringLiteral(" EVENT `%1`.`%2` ").arg(tgtDb, e));
            if(!ddl.isEmpty())
                stmts << ddl;
        }
    }

    stmts << QStringLiteral("SET FOREIGN_KEY_CHECKS=1");

    bool ok = true;
    for(const QString &s : std::as_const(stmts)) {
        if(mysql_query(m_conn, s.toUtf8().constData()) != 0) {
            if(error)
                *error = QStringLiteral("%1\n  at: %2")
                             .arg(QString::fromUtf8(mysql_error(m_conn)), s);
            ok = false;
            break;
        }
    }
    mysql_query(m_conn, "SET FOREIGN_KEY_CHECKS=1");
    /* the "USE `tgt`" statement left the browsing connection on the target db;
     * put it back on this tab's database */
    if(!m_params.database.isEmpty()) {
        wyString use;
        use.Sprintf("USE `%s`", m_params.database.toUtf8().constData());
        mysql_query(m_conn, use.GetString());
    }
    return ok;
}

void ConnectionTab::promptUserManager()
{
    if(m_conn)
        UserManagerDialog(m_conn, this).exec();
}

void ConnectionTab::promptImportXml(const QString &database, const QString &table)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? m_params.database : database;

    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import XML — pick a file"), QString(),
        QStringLiteral("XML (*.xml);;All files (*)"));
    if(file.isEmpty())
        return;

    QStringList tbls;
    wyString sq;
    sq.Sprintf("SHOW TABLES FROM `%s`",
               QString(db).replace('`', QStringLiteral("``")).toUtf8().constData());
    if(mysql_query(m_conn, sq.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res))
                if(row[0]) tbls << QString::fromUtf8(row[0]);
            mysql_free_result(res);
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Import XML into `%1`").arg(db));
    auto *tbl = new QComboBox(&dlg);
    tbl->addItems(tbls);
    if(!table.isEmpty())
        tbl->setCurrentText(table);
    auto *rowTag = new QLineEdit(QStringLiteral("row"), &dlg);
    auto *truncate = new QCheckBox(QStringLiteral("Empty the table first"), &dlg);
    auto *replace = new QCheckBox(QStringLiteral("REPLACE existing rows (by key)"), &dlg);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Target table"), tbl);
    form->addRow(QStringLiteral("Rows identified by  <tag>"), rowTag);
    form->addRow(QString(), truncate);
    form->addRow(QString(), replace);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Import"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(new QLabel(QStringLiteral(
        "Uses LOAD XML LOCAL INFILE — the server must allow local-infile."),
        &dlg));
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted || tbl->currentText().isEmpty())
        return;

    const QString target = tbl->currentText();
    const auto esc = [](QString s) {
        return s.replace('\\', QStringLiteral("\\\\"))
                .replace('\'', QStringLiteral("\\'"));
    };
    const QString tag = rowTag->text().trimmed().isEmpty()
        ? QStringLiteral("row") : rowTag->text().trimmed();
    if(truncate->isChecked())
        execDdl(QStringLiteral("TRUNCATE TABLE `%1`.`%2`").arg(db, target));

    const QString sql = QStringLiteral(
        "LOAD XML LOCAL INFILE '%1' %2 INTO TABLE `%3`.`%4` "
        "CHARACTER SET utf8mb4 ROWS IDENTIFIED BY '<%5>'")
        .arg(esc(file),
             replace->isChecked() ? QStringLiteral("REPLACE") : QStringLiteral("IGNORE"),
             db, target, esc(tag));
    wyString q;
    q.SetAs(sql.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) != 0) {
        m_messages->setPlainText(QStringLiteral("XML import failed: %1")
                                     .arg(QString::fromUtf8(mysql_error(m_conn))));
    } else {
        const char *info = mysql_info(m_conn);
        m_messages->setPlainText(QStringLiteral("Imported into `%1`.`%2` — %3")
            .arg(db, target,
                 info ? QString::fromUtf8(info)
                      : QStringLiteral("%1 row(s)")
                            .arg((long long)mysql_affected_rows(m_conn))));
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
    const QString db = database.isEmpty() ? m_params.database : database;

    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import CSV — pick a file"), QString(),
        QStringLiteral("CSV / text (*.csv *.tsv *.txt);;All files (*)"));
    if(file.isEmpty())
        return;

    /* target table + parse options */
    QStringList tbls;
    wyString sq;
    sq.Sprintf("SHOW TABLES FROM `%s`",
               QString(db).replace('`', QStringLiteral("``")).toUtf8().constData());
    if(mysql_query(m_conn, sq.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res))
                if(row[0]) tbls << QString::fromUtf8(row[0]);
            mysql_free_result(res);
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Import CSV into `%1`").arg(db));
    auto *tbl = new QComboBox(&dlg);
    tbl->addItems(tbls);
    if(!table.isEmpty())
        tbl->setCurrentText(table);
    auto *fieldSep = new QLineEdit(QStringLiteral(","), &dlg);
    auto *enclosure = new QLineEdit(QStringLiteral("\""), &dlg);
    auto *lineSep = new QComboBox(&dlg);
    lineSep->addItems({ QStringLiteral("\\n  (Unix)"), QStringLiteral("\\r\\n  (Windows)") });
    auto *header = new QCheckBox(QStringLiteral("First line holds column names"), &dlg);
    header->setChecked(true);
    auto *truncate = new QCheckBox(QStringLiteral("Empty the table first"), &dlg);
    auto *replace = new QCheckBox(QStringLiteral("REPLACE existing rows (by key)"), &dlg);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Target table"), tbl);
    form->addRow(QStringLiteral("Field separator"), fieldSep);
    form->addRow(QStringLiteral("Quote character"), enclosure);
    form->addRow(QStringLiteral("Line separator"), lineSep);
    form->addRow(QString(), header);
    form->addRow(QString(), truncate);
    form->addRow(QString(), replace);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Import"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(new QLabel(QStringLiteral(
        "Uses LOAD DATA LOCAL INFILE — the server must allow local-infile."),
        &dlg));
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted || tbl->currentText().isEmpty())
        return;

    const QString target = tbl->currentText();
    const QString sep = fieldSep->text().isEmpty() ? QStringLiteral(",")
                                                   : fieldSep->text();
    const QString quote = enclosure->text();
    const auto esc = [](QString s) {
        return s.replace('\\', QStringLiteral("\\\\"))
                .replace('\'', QStringLiteral("\\'"));
    };

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
        execDdl(QStringLiteral("TRUNCATE TABLE `%1`.`%2`").arg(db, target));

    QString sql = QStringLiteral(
        "LOAD DATA LOCAL INFILE '%1' %2 INTO TABLE `%3`.`%4` "
        "CHARACTER SET utf8mb4 "
        "FIELDS TERMINATED BY '%5' ENCLOSED BY '%6' "
        "LINES TERMINATED BY '%7'%8%9")
        .arg(esc(file),
             replace->isChecked() ? QStringLiteral("REPLACE") : QStringLiteral("IGNORE"),
             db, target, esc(sep), esc(quote),
             lineSep->currentIndex() == 1 ? QStringLiteral("\\r\\n")
                                          : QStringLiteral("\\n"),
             header->isChecked() ? QStringLiteral(" IGNORE 1 LINES") : QString(),
             colList);

    wyString q;
    q.SetAs(sql.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) != 0) {
        m_messages->setPlainText(QStringLiteral("Import failed: %1")
                                     .arg(QString::fromUtf8(mysql_error(m_conn))));
    } else {
        const char *info = mysql_info(m_conn);
        m_messages->setPlainText(QStringLiteral("Imported into `%1`.`%2` — %3")
            .arg(db, target,
                 info ? QString::fromUtf8(info)
                      : QStringLiteral("%1 row(s)")
                            .arg((long long)mysql_affected_rows(m_conn))));
        if(m_tableData->loadedTable() == target)
            m_tableData->load(m_conn, db, target);
    }
    m_resultTabs->setCurrentWidget(m_messages);
    refreshBrowser();
}

void ConnectionTab::promptManageForeignKeys(const QString &database,
                                            const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? m_params.database : database;

    QList<ForeignKeyDialog::FkDef> fks;
    const auto find = [&](const QString &n) -> ForeignKeyDialog::FkDef * {
        for(auto &f : fks)
            if(f.name == n)
                return &f;
        return nullptr;
    };
    wyString q;
    q.Sprintf(
        "SELECT k.CONSTRAINT_NAME, k.COLUMN_NAME, k.REFERENCED_TABLE_NAME, "
        "k.REFERENCED_COLUMN_NAME, r.DELETE_RULE, r.UPDATE_RULE "
        "FROM information_schema.KEY_COLUMN_USAGE k "
        "JOIN information_schema.REFERENTIAL_CONSTRAINTS r "
        "  ON r.CONSTRAINT_SCHEMA=k.CONSTRAINT_SCHEMA "
        "  AND r.CONSTRAINT_NAME=k.CONSTRAINT_NAME "
        "WHERE k.TABLE_SCHEMA='%s' AND k.TABLE_NAME='%s' "
        "  AND k.REFERENCED_TABLE_NAME IS NOT NULL "
        "ORDER BY k.CONSTRAINT_NAME, k.ORDINAL_POSITION",
        db.toUtf8().constData(), table.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) != 0) {
        QMessageBox::warning(this, QStringLiteral("Foreign Keys"),
                             QString::fromUtf8(mysql_error(m_conn)));
        return;
    }
    if(MYSQL_RES *res = mysql_store_result(m_conn)) {
        while(MYSQL_ROW row = mysql_fetch_row(res)) {
            const QString name = QString::fromUtf8(row[0] ? row[0] : "");
            ForeignKeyDialog::FkDef *f = find(name);
            if(!f) {
                ForeignKeyDialog::FkDef nf;
                nf.name = name;
                nf.refTable = QString::fromUtf8(row[2] ? row[2] : "");
                nf.onDelete = QString::fromUtf8(row[4] ? row[4] : "RESTRICT");
                nf.onUpdate = QString::fromUtf8(row[5] ? row[5] : "RESTRICT");
                fks << nf;
                f = &fks.last();
            }
            f->columns << QString::fromUtf8(row[1] ? row[1] : "");
            f->refColumns << QString::fromUtf8(row[3] ? row[3] : "");
        }
        mysql_free_result(res);
    }

    QStringList cols, tables;
    q.Sprintf("SHOW COLUMNS FROM `%s`.`%s`", db.toUtf8().constData(),
              table.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res))
                if(row[0]) cols << QString::fromUtf8(row[0]);
            mysql_free_result(res);
        }
    }
    q.Sprintf("SHOW TABLES FROM `%s`", db.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res))
                if(row[0]) tables << QString::fromUtf8(row[0]);
            mysql_free_result(res);
        }
    }

    ForeignKeyDialog dlg(db, table, fks, cols, tables, this);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    if(sql.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Foreign Keys"),
                                QStringLiteral("No changes to apply."));
        return;
    }
    execDdl(sql);
}

void ConnectionTab::promptAlterTable(const QString &database,
                                     const QString &table)
{
    if(!m_conn || table.isEmpty())
        return;
    const QString db = database.isEmpty() ? m_params.database : database;

    /* columns via SHOW FULL COLUMNS: Field Type Collation Null Key Default
     * Extra Privileges Comment */
    QList<CreateTableDialog::ColumnDef> cols;
    wyString q;
    q.Sprintf("SHOW FULL COLUMNS FROM `%s`.`%s`", db.toUtf8().constData(),
              table.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) != 0) {
        QMessageBox::warning(this, QStringLiteral("Alter Table"),
                             QString::fromUtf8(mysql_error(m_conn)));
        return;
    }
    if(MYSQL_RES *res = mysql_store_result(m_conn)) {
        while(MYSQL_ROW row = mysql_fetch_row(res)) {
            CreateTableDialog::ColumnDef c;
            c.name = QString::fromUtf8(row[0] ? row[0] : "");
            QString type = QString::fromUtf8(row[1] ? row[1] : "").trimmed();
            c.isUnsigned = type.contains(QStringLiteral(" unsigned"),
                                         Qt::CaseInsensitive);
            type.remove(QStringLiteral(" unsigned"), Qt::CaseInsensitive);
            type.remove(QStringLiteral(" zerofill"), Qt::CaseInsensitive);
            const int lp = type.indexOf('(');
            if(lp >= 0 && type.endsWith(')')) {
                c.length = type.mid(lp + 1, type.size() - lp - 2);
                c.type = type.left(lp).toUpper();
            } else {
                c.type = type.toUpper();
            }
            c.notNull = QString::fromUtf8(row[3] ? row[3] : "") == QStringLiteral("NO");
            c.pk = QString::fromUtf8(row[4] ? row[4] : "") == QStringLiteral("PRI");
            c.def = QString::fromUtf8(row[5] ? row[5] : "");
            c.autoInc = QString::fromUtf8(row[6] ? row[6] : "")
                            .contains(QStringLiteral("auto_increment"),
                                      Qt::CaseInsensitive);
            c.comment = QString::fromUtf8(row[8] ? row[8] : "");
            cols << c;
        }
        mysql_free_result(res);
    }
    if(cols.isEmpty())
        return;

    QString engine, charset;
    q.Sprintf("SELECT ENGINE, SUBSTRING_INDEX(TABLE_COLLATION,'_',1) "
              "FROM information_schema.TABLES "
              "WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s'",
              db.toUtf8().constData(), table.toUtf8().constData());
    if(mysql_query(m_conn, q.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            if(MYSQL_ROW row = mysql_fetch_row(res)) {
                engine  = QString::fromUtf8(row[0] ? row[0] : "");
                charset = QString::fromUtf8(row[1] ? row[1] : "");
            }
            mysql_free_result(res);
        }
    }

    CreateTableDialog dlg(db, table, cols, engine, charset, this);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString sql = dlg.buildSql();
    if(sql.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Alter Table"),
            QStringLiteral("No changes to apply."));
        return;
    }
    execDdl(sql);
    if(m_tableData->loadedTable() == table)
        m_tableData->load(m_conn, db, table);
}

void ConnectionTab::promptDumpDatabase(const QString &database)
{
    if(!m_conn)
        return;
    const QString db = database.isEmpty() ? m_params.database : database;
    if(db.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Backup As SQL Dump"),
            QStringLiteral("Select a database first."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Backup `%1` as SQL dump").arg(db),
        db + QStringLiteral(".sql"), QStringLiteral("SQL (*.sql);;All (*)"));
    if(path.isEmpty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    const bool ok = dumpDatabaseToFile(db, path, &err);
    QApplication::restoreOverrideCursor();

    m_messages->setPlainText(ok
        ? QStringLiteral("Dumped `%1` → %2  (%3 KB)")
              .arg(db, path).arg((QFileInfo(path).size() + 1023) / 1024)
        : QStringLiteral("Dump failed: %1").arg(err));
    m_resultTabs->setCurrentWidget(m_messages);
}

bool ConnectionTab::dumpDatabaseToFile(const QString &database,
                                      const QString &path, QString *error)
{
    if(!m_conn) {
        if(error) *error = QStringLiteral("not connected");
        return false;
    }
    const QString db = database.isEmpty() ? m_params.database : database;
    QFile f(path);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if(error) *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    const bool ok = SqlDump::write(m_conn, db, {}, SqlDump::Options{}, &f, error);
    f.close();
    return ok;
}

bool ConnectionTab::execDdl(const QString &sql)
{
    if(!m_conn)
        return false;
    wyString q;
    q.SetAs(sql.toUtf8().constData());
    const bool ok = mysql_query(m_conn, q.GetString()) == 0;
    m_messages->appendPlainText(ok ? QStringLiteral("OK: ") + sql
                                   : QStringLiteral("Error: ")
                                         + mysql_error(m_conn));
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

    QStringList cols;
    wyString q;
    q.Sprintf("SHOW COLUMNS FROM `%s`.`%s`", db.toUtf8().constData(),
              table.toUtf8().constData());
    if(m_conn && mysql_query(m_conn, q.GetString()) == 0) {
        if(MYSQL_RES *res = mysql_store_result(m_conn)) {
            while(MYSQL_ROW row = mysql_fetch_row(res))
                if(row[0])
                    cols << QString::fromUtf8(row[0]);
            mysql_free_result(res);
        }
    }
    if(cols.isEmpty())
        return;

    const QString colsB = '`' + cols.join("`, `") + '`';
    QString stmt;
    switch(kind) {
    case 0: {
        QStringList marks;
        for(int i = 0; i < cols.size(); ++i)
            marks << QStringLiteral("?");
        stmt = QStringLiteral("INSERT INTO `%1`.`%2` (%3)\nVALUES (%4);")
                   .arg(db, table, colsB, marks.join(", "));
        break;
    }
    case 1: {
        QStringList sets;
        for(const QString &c : cols)
            sets << QStringLiteral("`%1` = '?'").arg(c);
        stmt = QStringLiteral("UPDATE `%1`.`%2` SET %3\nWHERE <condition>;")
                   .arg(db, table, sets.join(", "));
        break;
    }
    case 2:
        stmt = QStringLiteral("DELETE FROM `%1`.`%2`\nWHERE <condition>;")
                   .arg(db, table);
        break;
    default:
        stmt = QStringLiteral("SELECT %1\nFROM `%2`.`%3`;")
                   .arg(colsB, db, table);
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

void ConnectionTab::useDatabase(const QString &db)
{
    if(!m_conn)
        return;
    wyString u;
    u.Sprintf("USE `%s`", db.toUtf8().constData());
    if(mysql_query(m_conn, u.GetString()) == 0) {
        m_params.database = db;
        m_messages->setPlainText(QStringLiteral("Database changed to %1").arg(db));
        m_resultTabs->setCurrentWidget(m_messages);
        updateCompletions();
    } else {
        m_messages->setPlainText(QString::fromUtf8(mysql_error(m_conn)));
    }
}
