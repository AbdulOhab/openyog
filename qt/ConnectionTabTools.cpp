/* ConnectionTab, the Tools/Database menu actions that don't change schema: SQL dump,
 * user manager, table diagnostics and properties, schema HTML, data
 * search, and exporting table data.
 * The class is declared in ConnectionTab.h; ConnectionTab.cpp holds the
 * core (editors, query execution, results, object browser). */
#include "ConnectionTab.h"
#include "TableDataView.h"

#include "ExportDialog.h"
#include "ResultExport.h"
#include "UserManagerDialog.h"
#include "SqlDump.h"
#include "SqlDumpDialog.h"
#include "db/IDbDriver.h"
#include "db/IDbConnection.h"

#include <QPlainTextEdit>
#include <QApplication>

#include <QDir>
#include <QFileDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QSet>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QFile>
#include <QFileInfo>
#include <QVBoxLayout>
#include <utility>

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
    runDumpDialog(db, {});
}

/* the Backup … As SQL Dump flow shared by the Database, Table and Tools
 * entries: one options dialog (upstream's own option set), then either one
 * file or — "file per object" — one <table>.sql per table inside a directory */
void ConnectionTab::runDumpDialog(const QString &db, const QStringList &preselect)
{
    SqlDumpDialog dlg(m_conn, db, preselect, this);
    if(dlg.exec() != QDialog::Accepted)
        return;
    const QString path = dlg.targetPath();
    if(path.isEmpty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    bool ok;
    QString done;
    if(dlg.filePerObject()) {
        QStringList tables = dlg.selectedTables();
        if(tables.isEmpty())
            tables = m_conn->listTables(db, QStringLiteral("BASE TABLE"));
        QDir().mkpath(path);
        const QString stamp = dlg.timestampPrefix();
        int n = 0;
        ok = true;
        for(const QString &t : std::as_const(tables)) {
            const QString file = QDir(path).filePath(stamp + t + QStringLiteral(".sql"));
            if(!(ok = dumpDatabaseToFile(db, file, {t}, dlg.options(), &err)))
                break;
            ++n;
        }
        done = QStringLiteral("Dumped %1 table(s) of `%2` \u2192 %3").arg(n).arg(db, path);
    } else {
        ok = dumpDatabaseToFile(db, path, dlg.selectedTables(), dlg.options(), &err);
        done = QStringLiteral("Dumped `%1` \u2192 %2  (%3 KB)")
                   .arg(db, path)
                   .arg((QFileInfo(path).size() + 1023) / 1024);
    }
    QApplication::restoreOverrideCursor();
    m_messages->setPlainText(ok ? done : QStringLiteral("Dump failed: %1").arg(err));
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
