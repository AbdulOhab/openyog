#include "ConnectionTab.h"
#include "ObjectBrowser.h"
#include "TableDataView.h"
#include "SqlHighlighter.h"
#include "Icons.h"
#include "wyString.h"

#include <QElapsedTimer>
#include <QFileDialog>
#include <QFontDatabase>
#include <QTextStream>
#include <QHeaderView>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSplitter>
#include <QTabBar>
#include <QTextStream>
#include <QFile>
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
    attachEditor(m_editor, QStringLiteral("Query 1"));

    m_history = new QPlainTextEdit(this);
    m_history->setReadOnly(true);
    m_history->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

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
    m_editorTabs->addTab(m_history, Icons::get(QStringLiteral("history.ico")),
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

    auto *editorSide = new QWidget(this);
    auto *editorCol = new QVBoxLayout(editorSide);
    editorCol->setContentsMargins(0, 0, 0, 0);
    editorCol->setSpacing(0);
    editorCol->addWidget(m_infoBar);
    editorCol->addWidget(m_editorTabs, 1);

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
            [this](const QString &db, const QString &table) {
        if(QMessageBox::question(this, QStringLiteral("Drop Table"),
                QStringLiteral("Permanently DROP table `%1`.`%2`?")
                    .arg(db, table)) != QMessageBox::Yes)
            return;
        execDdl(QStringLiteral("DROP TABLE `%1`.`%2`").arg(db, table));
        m_tableData->clear();
    });
    connect(m_browser, &ObjectBrowser::truncateTableRequested, this,
            [this](const QString &db, const QString &table) {
        if(QMessageBox::question(this, QStringLiteral("Truncate Table"),
                QStringLiteral("Delete ALL rows of `%1`.`%2`?")
                    .arg(db, table)) != QMessageBox::Yes)
            return;
        execDdl(QStringLiteral("TRUNCATE TABLE `%1`.`%2`").arg(db, table));
        if(m_tableData->loadedTable() == table)
            m_tableData->load(m_conn, db, table);   /* empty grid */
    });

    m_infoBar->setText(QStringLiteral(
        "OpenYog — connected to %1@%2:%3%4")
        .arg(m_params.user, m_params.host).arg(m_params.port)
        .arg(m_params.database.isEmpty() ? QString()
                                         : QStringLiteral("/") + m_params.database));

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
    Q_UNUSED(title);
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
    m_history->appendPlainText(
        QStringLiteral("[%1] %2")
            .arg(QTime::currentTime().toString(QStringLiteral("hh:mm:ss")), sql));
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
    auto *grid = new QTableView(this);
    grid->setModel(model);
    grid->horizontalHeader()->setStretchLastSection(true);
    grid->setAlternatingRowColors(true);

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
    m_editorTabs->setCurrentWidget(m_history);
}

void ConnectionTab::exportResultCsv()
{
    QAbstractItemModel *m = m_lastGrid ? m_lastGrid->model() : nullptr;
    if(!m || m->rowCount() == 0) {
        m_messages->appendPlainText(QStringLiteral("no result set to export"));
        return;
    }
    const QString f = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export result as CSV"),
        QStringLiteral("result.csv"), QStringLiteral("CSV (*.csv)"));
    if(f.isEmpty())
        return;
    QFile file(f);
    if(!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream out(&file);
    QStringList header;
    for(int c = 0; c < m->columnCount(); ++c)
        header << '"' + m->headerData(c, Qt::Horizontal).toString() + '"';
    out << header.join(',') << "\n";
    for(int r = 0; r < m->rowCount(); ++r) {
        QStringList row;
        for(int c = 0; c < m->columnCount(); ++c)
            row << '"' + m->index(r, c).data().toString() + '"';
        out << row.join(',') << "\n";
    }
    m_messages->appendPlainText(
        QStringLiteral("Exported %1 rows to %2").arg(m->rowCount()).arg(f));
}

void ConnectionTab::refreshBrowser()
{
    if(m_conn)
        m_browser->loadDatabases(m_conn, m_params.database);
}

void ConnectionTab::editTableCell(int row, int col, const QString &value)
{
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
    } else {
        m_messages->setPlainText(QString::fromUtf8(mysql_error(m_conn)));
    }
}
