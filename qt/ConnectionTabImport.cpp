/* ConnectionTab, importing CSV and XML files into a table.
 * The class is declared in ConnectionTab.h; ConnectionTab.cpp holds the
 * core (editors, query execution, results, object browser). */
#include "ConnectionTab.h"
#include "DelimitedText.h"
#include "TableDataView.h"

#include "db/IDbDriver.h"
#include "db/IDbConnection.h"

#include <QPushButton>
#include <QFileDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QFontDatabase>
#include <QMessageBox>
#include <QSpinBox>
#include <QFile>
#include <QVBoxLayout>

#include <algorithm>

namespace {

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

} // namespace

bool ConnectionTab::importCsvBatched(const QString &db, const QString &table, const QString &file,
                                     const QString &sep, const QString &quote, const QString &escCh,
                                     bool hasHeader, int extraSkipLines, bool truncateFirst,
                                     const QString &onDup, int *rowsInserted, QString *error,
                                     const QStringList &onlyCols)
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

    /* the dialog's column checklist: which of the table's columns this
     * import fills. With a header row the CSV fields are matched by NAME;
     * without one, by POSITION against the table's own column order —
     * exactly LOAD DATA's two mapping modes. Empty = every column (the
     * previous behavior). */
    QVector<int> sel;
    if(!onlyCols.isEmpty()) {
        if(!colNames.isEmpty()) {
            QStringList keep;
            for(int i = 0; i < colNames.size(); ++i) {
                if(onlyCols.contains(colNames.at(i), Qt::CaseInsensitive)) {
                    sel << i;
                    keep << colNames.at(i);
                }
            }
            colNames = keep;
        } else {
            QStringList all;
            for(const QStringList &row : m_conn->listColumns(db, table).rows)
                all << row.value(0);
            for(const QString &c : onlyCols) {
                const int idx = all.indexOf(c);
                if(idx >= 0)
                    sel << idx;
            }
            colNames = onlyCols;
        }
    }

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
        if(sel.isEmpty()) {
            for(const QString &v : row)
                vals << qv(v);
        } else {
            for(const int idx : sel)
                vals << qv(row.value(idx));
        }
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

    /* upstream's "Select Table and Columns To Import" checklist: which of
     * the target table's columns this import fills (all checked = every
     * column, the previous behavior). Rebuilt when the target table
     * changes. */
    auto *colPick = new QListWidget(&dlg);
    colPick->setMaximumHeight(120);
    const auto reloadCols = [this, db, tbl, colPick] {
        colPick->clear();
        for(const QStringList &row : m_conn->listColumns(db, tbl->currentText()).rows) {
            auto *it = new QListWidgetItem(row.value(0), colPick);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(Qt::Checked);
        }
    };
    reloadCols();
    connect(tbl, &QComboBox::currentTextChanged, reloadCols);

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
    lay->addWidget(new QLabel(QStringLiteral("Columns to import (all, when none checked):"), &dlg));
    lay->addWidget(colPick);
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

    /* the checklist's verdict: unchecked → every column (previous
     * behavior); else the checked subset, name-matched when the file has a
     * header and position-matched to the table's own order when not */
    QStringList onlyCols;
    for(int i = 0; i < colPick->count(); ++i)
        if(colPick->item(i)->checkState() == Qt::Checked)
            onlyCols << colPick->item(i)->text();
    if(onlyCols.size() == colPick->count())
        onlyCols.clear();

    if(noBulkLoader) {
        int rows = 0;
        QString err;
        const bool ok = importCsvBatched(
            db, target, file, sep, quote, escChar->text(), header->isChecked(), skipLines->value(),
            truncate->isChecked(), onDup->currentData().toString(), &rows, &err, onlyCols);
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
     * positionally into every column (wrong for an AUTO_INCREMENT-first table).
     * The checklist narrows both modes: with a header the kept names are the
     * CSV columns that are also checked; without one, the checked table
     * columns in the table's own order (positional). */
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
                if(onlyCols.isEmpty() || onlyCols.contains(c, Qt::CaseInsensitive))
                    cols << QStringLiteral("`%1`").arg(c.replace('`', QStringLiteral("``")));
            }
            if(!cols.isEmpty())
                colList = QStringLiteral(" (%1)").arg(cols.join(QStringLiteral(", ")));
        }
    } else if(!onlyCols.isEmpty()) {
        QStringList q;
        for(const QString &c : onlyCols)
            q << m_conn->quoteIdent(c);
        if(!q.isEmpty())
            colList = QStringLiteral(" (%1)").arg(q.join(QStringLiteral(", ")));
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
