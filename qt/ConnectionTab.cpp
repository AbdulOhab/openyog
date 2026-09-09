#include "ConnectionTab.h"
#include "ObjectBrowser.h"
#include "SqlHighlighter.h"
#include "wyString.h"

#include <QElapsedTimer>
#include <QFontDatabase>
#include <QHeaderView>
#include <QMetaObject>
#include <QPushButton>
#include <QSplitter>
#include <QTabBar>
#include <QTime>
#include <QVBoxLayout>
#include <utility>

#include <mysql/mysql.h>

#include <algorithm>
#include <thread>

namespace {

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
    new SqlHighlighter(m_editor->document());

    m_history = new QPlainTextEdit(this);
    m_history->setReadOnly(true);
    m_history->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_editorTabs = new QTabWidget(this);
    m_editorTabs->setDocumentMode(true);
    m_editorTabs->addTab(m_editor, QStringLiteral("Query 1"));
    m_editorTabs->addTab(m_history, QStringLiteral("History"));

    /* ---- right-bottom: result tabs -------------------------------- */
    m_messages = new QPlainTextEdit(this);
    m_messages->setReadOnly(true);

    m_info = new QLabel(QStringLiteral("Run a query to see server info."), this);
    m_info->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_info->setWordWrap(true);

    m_resultTabs = new QTabWidget(this);
    m_resultTabs->setDocumentMode(true);
    m_resultTabs->setTabPosition(QTabWidget::North);
    m_resultTabs->addTab(m_messages, QStringLiteral("1_Messages"));
    m_resultTabs->addTab(m_info,     QStringLiteral("3_Info"));
    m_resultTabs->setCurrentIndex(0);

    auto *rightSplit = new QSplitter(Qt::Vertical, this);
    rightSplit->addWidget(m_editorTabs);
    rightSplit->addWidget(m_resultTabs);
    rightSplit->setStretchFactor(0, 1);
    rightSplit->setStretchFactor(1, 1);

    auto *mainSplit = new QSplitter(Qt::Horizontal, this);
    mainSplit->addWidget(m_browser);
    mainSplit->addWidget(rightSplit);
    mainSplit->setStretchFactor(0, 0);
    mainSplit->setStretchFactor(1, 1);
    mainSplit->setSizes({260, 900});

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(mainSplit);

    /* ---- open the connection ------------------------------------- */
    m_conn = mysql_init(nullptr);
    mysql_options(m_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
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

    m_browser->setConnectionLabel(
        QStringLiteral("%1@%2").arg(m_params.user, m_params.host));
    m_browser->loadDatabases(m_conn, m_params.database);
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

void ConnectionTab::logHistory(const QString &sql)
{
    m_history->appendPlainText(
        QStringLiteral("[%1] %2")
            .arg(QTime::currentTime().toString(QStringLiteral("hh:mm:ss")), sql));
}

void ConnectionTab::runQuery()
{
    if(m_running) {
        m_messages->appendPlainText(QStringLiteral("a batch is already running…"));
        return;
    }
    const QStringList statements = splitStatements(m_editor->toPlainText());
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
    std::thread([guard, p, statements] {
        const QVector<QueryResult> results = runOnConnection(p, statements);
        QMetaObject::invokeMethod(guard, [guard, results] {
            if(guard)
                guard->applyResults(results, QStringLiteral("Result"));
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
    m_running = true;
    m_messages->setPlainText(QStringLiteral("Opening %1.%2…").arg(db, table));

    QPointer<ConnectionTab> guard(this);
    const ConnectionParams p = m_params;
    std::thread([guard, p, sql, db, table] {
        const QVector<QueryResult> results =
            runOnConnection(p, QStringList{ sql });
        QMetaObject::invokeMethod(guard, [guard, results, db, table] {
            if(guard)
                guard->applyResults(results,
                                    QStringLiteral("%1.%2").arg(db, table));
        }, Qt::QueuedConnection);
    }).detach();
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
    auto *grid = new QTableView(this);
    grid->setModel(model);
    grid->horizontalHeader()->setStretchLastSection(true);
    grid->setAlternatingRowColors(true);

    m_resultTabs->addTab(grid, title);
    m_dynamicResultTabs.append(grid);
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
    } else {
        m_messages->setPlainText(QString::fromUtf8(mysql_error(m_conn)));
    }
}
