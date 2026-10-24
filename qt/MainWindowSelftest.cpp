/* OpenYog — main window: the hooks qt/main.cpp's selftest flags drive it
 * through. The class is declared in MainWindow.h; MainWindow.cpp holds the
 * core. */
#include "MainWindow.h"
#include "ConnectionDialog.h"
#include "ConnectionTab.h"
#include "ObjectBrowser.h"
#include "DataSync.h"
#include "DataSyncDialog.h"
#include "ExternalDataDialog.h"
#include "QueryBuilder.h"
#include "MongoJson.h"
#include "MongoTab.h"
#include "SchemaDesigner.h"
#include "SchemaSyncDialog.h"
#include "SqlSplit.h"
#include "SessionStore.h"
#include "db/IDbConnection.h"
#include "db/MongoClient.h"

#include <QApplication>
#include <QFile>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <memory>

void MainWindow::editTableCell(int row, int col, const QString &value, bool stageOnly)
{
    if(auto *tab = currentTab())
        tab->editTableCell(row, col, value, stageOnly);
}

bool MainWindow::selftestDump(const QString &path)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    QString err;
    const bool ok = tab->dumpDatabaseToFile({}, path, &err);
    if(!ok)
        qWarning("dump failed: %s", qPrintable(err));
    return ok;
}

bool MainWindow::selftestCopyDb(const QString &src, const QString &tgt)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    QString err;
    const bool ok = tab->copyDatabaseTo(src, tgt, true, true, true, &err);
    if(!ok)
        qWarning("copydb failed: %s", qPrintable(err));
    return ok;
}

bool MainWindow::selftestCopyDbPostgres(const QString &src, const QString &tgt)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    QString err;
    const bool ok = tab->copyDatabaseToPostgres(src, tgt, true, true, true, &err);
    if(!ok)
        qWarning("pgcopydb failed: %s", qPrintable(err));
    return ok;
}

bool MainWindow::selftestMultiDb(const QString &otherDb)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    int fails = 0;
    const auto check = [&](bool ok, const QString &what) {
        if(!ok)
            ++fails;
        QTextStream(stdout) << "pgmultidbtest " << what << (ok ? "  PASS\n" : "  FAIL\n");
    };

    const QString primary = tab->currentDatabase();
    QString err1, err2, err3;
    IDbConnection *primaryViaEmpty = tab->connectionFor(QString(), &err1);
    IDbConnection *primaryViaName = tab->connectionFor(primary, &err2);
    check(primaryViaEmpty && primaryViaEmpty == primaryViaName,
          QStringLiteral(
              "connectionFor(\"\")/connectionFor(primary) both resolve to the same connection"));

    IDbConnection *side1 = tab->connectionFor(otherDb, &err3);
    check(side1 != nullptr, QStringLiteral("connectionFor(\"%1\") succeeds").arg(otherDb));
    if(!side1) {
        QTextStream(stdout) << "  error: " << err3 << '\n';
        return fails == 0;
    }
    check(side1 != primaryViaEmpty, QStringLiteral("side connection differs from the primary one"));

    QString err4;
    IDbConnection *side2 = tab->connectionFor(otherDb, &err4);
    check(side1 == side2,
          QStringLiteral("a second connectionFor() call for the same db reuses the cached one"));

    /* the side connection must genuinely see the OTHER database's own
     * catalog, not the primary's (the whole point of this feature) */
    const QStringList tables =
        side1->listTables(QStringLiteral("public"), QStringLiteral("BASE TABLE"));
    check(!tables.isEmpty(),
          QStringLiteral("side connection lists tables in %1.public").arg(otherDb));
    QTextStream(stdout) << "  tables: " << tables.join(QStringLiteral(", ")) << '\n';

    const ConnectionParams p = tab->paramsFor(otherDb);
    check(p.database == otherDb && p.host == tab->params().host && p.user == tab->params().user &&
              p.driverType == tab->driverType(),
          QStringLiteral("paramsFor() carries the right database with the same host/user/driver"));

    /* end-to-end: this is what "Connect to <db> in New Tab" actually does
     * (ObjectBrowser::openDatabaseInNewTabRequested -> ConnectionTab::
     * newTabRequested -> this same openAndRun call, wired in openAndRun()
     * itself) — verify it produces a second, correctly-scoped tab rather
     * than just trusting the two already-tested halves compose correctly */
    const int tabsBefore = m_tabs->count();
    const bool opened = openAndRun(p);
    check(opened && m_tabs->count() == tabsBefore + 1,
          QStringLiteral("\"Connect in New Tab\" opens exactly one new tab"));
    if(opened) {
        /* currentDatabase(), not defaultDb() — the latter is the *schema*
         * ("public" until the user picks another one), unrelated to which
         * physical database the new tab actually connected to */
        auto *newTab = qobject_cast<ConnectionTab *>(m_tabs->widget(m_tabs->count() - 1));
        check(
            newTab && newTab->currentDatabase() == otherDb,
            QStringLiteral("the new tab is scoped to %1, not the original database").arg(otherDb));
    }

    return fails == 0;
}

bool MainWindow::selftestCopySqliteFile(const QString &target, bool withData)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    QString err;
    const bool ok = tab->copySqliteFileTo(target, withData, &err);
    if(!ok)
        qWarning("sqlitecopydb failed: %s", qPrintable(err));
    return ok;
}

bool MainWindow::selftestSchemaHtml(const QString &outFile)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    const QString db = tab->defaultDb().isEmpty() ? QStringLiteral("main") : tab->defaultDb();
    const QString html = tab->buildSchemaHtml(db);
    QFile f(outFile);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("schemahtmltest: could not write %s", qPrintable(outFile));
        return false;
    }
    f.write(html.toUtf8());
    return true;
}

bool MainWindow::selftestCsvImportBatched(const QString &db, const QString &file,
                                          const QString &table, const QString &onDup,
                                          const QStringList &onlyCols)
{
    auto *tab = currentTab();
    if(!tab)
        return false;
    int rows = 0;
    QString err;
    const bool ok =
        tab->importCsvBatched(db, table, file, QStringLiteral(","), QStringLiteral("\""),
                              QStringLiteral("\\"), true, 0, false, onDup, &rows, &err, onlyCols);
    if(!ok)
        qWarning("csvimport failed: %s", qPrintable(err));
    else
        qInfo("csvimport: %d row(s) inserted into %s", rows, qPrintable(table));
    return ok;
}

void MainWindow::openTableData(const QString &db, const QString &table)
{
    if(auto *tab = currentTab())
        tab->openTableData(db, table);
}

void MainWindow::setDataViewMode(const QString &mode)
{
    if(auto *tab = currentTab())
        tab->setDataViewMode(mode);
}

QString MainWindow::selftestCellText(int row, int col) const
{
    if(auto *tab = currentTab())
        return tab->cellTextForTest(row, col);
    return QString();
}

QString MainWindow::selftestTdMenuDump(int row, int col)
{
    if(auto *tab = currentTab())
        return tab->tdMenuDumpForTest(row, col);
    return QString();
}

void MainWindow::openSchemaObjectTab(const QString &objType)
{
    if(auto *tab = currentTab())
        tab->createSchemaObject({}, objType);
}

void MainWindow::selftestUseDatabase(const QString &db)
{
    if(auto *tab = currentTab())
        tab->useDatabase(db);
}

void MainWindow::selftestExpandDatabase(const QString &name)
{
    if(auto *tab = currentTab())
        tab->expandDatabaseNode(name);
}

void MainWindow::selftestRunAgain()
{
    executeCurrentTab();
}

void MainWindow::selftestExplain(const QString &mode)
{
    if(auto *tab = currentTab())
        tab->selftestExplain(mode);
}

QString MainWindow::selftestShowDataTab()
{
    if(auto *tab = currentTab())
        return tab->selftestShowDataTab();
    return QString();
}

QString MainWindow::selftestInfoSearch(const QString &text)
{
    if(auto *tab = currentTab())
        return tab->selftestInfoSearch(text);
    return QString();
}

void MainWindow::selftestShowInfoTab()
{
    if(auto *tab = currentTab())
        tab->selftestShowInfoTab();
}

void MainWindow::selftestSelectBrowserItem(const QString &path)
{
    if(auto *tab = currentTab())
        tab->selectBrowserItem(path);
}

void MainWindow::selftestClickBrowserItem(const QString &path)
{
    if(auto *tab = currentTab())
        tab->clickBrowserItem(path);
}

void MainWindow::selftestCollapseBrowserItem(const QString &path)
{
    if(auto *tab = currentTab())
        tab->collapseBrowserItem(path);
}

void MainWindow::selftestExpandBrowserItem(const QString &path)
{
    if(auto *tab = currentTab())
        tab->expandBrowserItem(path);
}

QStringList MainWindow::selftestDumpTree(const QString &path)
{
    if(auto *tab = currentTab())
        return tab->dumpBrowserSubtree(path);
    return {};
}

QStringList MainWindow::selftestTreeMenu(const QString &path)
{
    if(auto *tab = currentTab())
        return tab->browserContextMenuItems(path);
    return {};
}

QStringList MainWindow::selftestDumpEditor()
{
    if(auto *tab = currentTab())
        return tab->dumpEditorText();
    return {};
}

void MainWindow::selftestSetEditorText(const QString &sql)
{
    if(auto *tab = currentTab())
        tab->setEditorTextForTest(sql);
}

QStringList MainWindow::selftestCompletions(const QString &sql)
{
    if(auto *tab = currentTab())
        return tab->completionsForTest(sql);
    return {};
}

void MainWindow::selftestDoubleClickBrowserItem(const QString &path)
{
    if(auto *tab = currentTab())
        tab->doubleClickBrowserItem(path);
}

QStringList MainWindow::selftestComboItems() const
{
    /* [0] is the combo's current selection — "(none)" when
     * currentIndex() is -1 (nothing picked yet), matching what the combo
     * visually shows (blank) in that state — followed by every item */
    QStringList out;
    out << (m_dbCombo->currentIndex() < 0 ? QStringLiteral("(none)") : m_dbCombo->currentText());
    for(int i = 0; i < m_dbCombo->count(); ++i)
        out << m_dbCombo->itemText(i);
    return out;
}

void MainWindow::selftestSwitchDatabase(const QString &db)
{
    /* switching databases is the Object Browser tree's job (double-click
     * a database node, or its "Switch to `db`" menu entry) — the toolbar
     * combo only ever lists schemas, so this calls ConnectionTab's own
     * method directly rather than driving tree widget items, which is
     * exactly what either of those real interactions ends up calling */
    if(auto *tab = currentTab())
        tab->switchDatabase(db);
}

int MainWindow::selftestTabCount() const
{
    return m_tabs->count();
}

void MainWindow::selftestPickDropdownSchema(const QString &schema)
{
    m_dbCombo->setCurrentText(schema);
}

bool MainWindow::selftestSessionRoundTrip(const ConnectionParams &params)
{
    bool ok = true;
    const auto check = [&](bool pass, const char *what) {
        ok = ok && pass;
        QTextStream(stdout) << "sessiontest " << what << (pass ? "  PASS\n" : "  FAIL\n");
    };
    SessionStore::setEditors(params.name, {}); /* whatever an earlier run left */
    if(!openAndRun(params))
        return false;
    ConnectionTab *tab = currentTab();
    tab->setEditorTextForTest(QStringLiteral("SELECT 1;\nSELECT 2;"));
    tab->openEditorWithSql(QStringLiteral("Totals"),
                           QStringLiteral("SELECT COUNT(*) FROM employees"));
    const QList<SessionStore::EditorState> before = tab->editorStates();

    closeTab(m_tabs->indexOf(tab));
    check(SessionStore::editors(params.name).size() == 2, "closing the tab saves both query tabs");

    if(!openAndRun(params))
        return false;
    const QList<SessionStore::EditorState> after = currentTab()->editorStates();
    check(after.size() == 2, "reopening restores them, without an extra empty Query 1");
    check(after.size() == 2 && after[0].title == before[0].title &&
              after[0].text == before[0].text && after[1].title == before[1].title &&
              after[1].text == before[1].text,
          "titles and text come back as they were");
    check(after.size() == 2 && after[0].cursor == before[0].cursor, "the caret comes back too");
    if(after.size() == 2)
        QTextStream(stdout) << "  caret before=" << before[0].cursor << " after=" << after[0].cursor
                            << '\n';

    saveSessionState();
    check(SessionStore::openConnections() == QStringList{params.name},
          "the open connection is remembered for the next start");
    return ok;
}

QStringList MainWindow::selftestSchemaDesigner(const QString &exportFile)
{
    auto *tab = currentTab();
    SchemaDesigner *designer = tab ? tab->openSchemaDesigner() : nullptr;
    if(!designer)
        return {QStringLiteral("schemadesigner: not opened")};
    QStringList out = designer->dumpForTest();
    if(!exportFile.isEmpty())
        out << QStringLiteral("schemadesigner export: %1")
                   .arg(designer->exportImage(exportFile) ? QStringLiteral("ok")
                                                          : QStringLiteral("FAILED"));
    return out;
}

QStringList MainWindow::selftestDrawForeignKey()
{
    auto *tab = currentTab();
    SchemaDesigner *designer = tab ? tab->openSchemaDesigner() : nullptr;
    if(!designer)
        return {QStringLiteral("drawfk: not opened")};
    const QString db = designer->database();
    IDbConnection *c = tab->connectionFor({}, nullptr);
    const QString parent = c->qualify(db, QStringLiteral("oy_fk_parent")),
                  child = c->qualify(db, QStringLiteral("oy_fk_child"));
    QStringList out;
    const auto run = [&](const QString &sql) {
        QString error;
        if(!c->query(sql, nullptr, &error))
            out << QStringLiteral("drawfk error: %1").arg(error);
    };
    const auto dropBoth = [&] {
        run(QStringLiteral("DROP TABLE IF EXISTS %1").arg(child));
        run(QStringLiteral("DROP TABLE IF EXISTS %1").arg(parent));
    };
    dropBoth();
    run(QStringLiteral("CREATE TABLE %1 (id INT PRIMARY KEY)").arg(parent));
    run(QStringLiteral("CREATE TABLE %1 (id INT PRIMARY KEY, parent_id INT)").arg(child));
    designer->reload();
    /* what the designer's relationshipDrawn does, the confirmation answered */
    const bool added =
        tab->addForeignKey(db, QStringLiteral("oy_fk_child"), QStringLiteral("parent_id"),
                           QStringLiteral("oy_fk_parent"), QStringLiteral("id"), false);
    out << QStringLiteral("drawfk: %1")
               .arg(added ? QStringLiteral("added") : QStringLiteral("refused"));
    designer->reload();
    for(const QString &line : designer->dumpForTest())
        if(line.contains(QStringLiteral("oy_fk_child")))
            out << line;
    dropBoth();
    designer->reload();
    return out;
}

QStringList MainWindow::selftestMongoTab()
{
    auto *tab = qobject_cast<MongoTab *>(m_tabs->currentWidget());
    if(!tab)
        return {QStringLiteral("mongotab: no MongoDB tab")};
    const QString db = QStringLiteral("openyog_test_mongo_ui");
    QStringList out;
    const auto canonical = [](const QString &shell) {
        MongoJson::Value v;
        MongoJson::parse(shell, &v);
        return MongoJson::toCanonical(v);
    };
    /* the data, straight through a client of its own */
    QString error;
    const auto seed = MongoClient::connect(tab->params(), &error);
    if(!seed)
        return {QStringLiteral("mongotab: seeding failed: ") + error};
    seed->dropDatabase(db, nullptr);
    seed->insert(db, QStringLiteral("people"),
                 {canonical("{_id: 1, name: 'Alice', age: 30, city: 'Dhaka', tags: ['a', 'b']}"),
                  canonical("{_id: 2, name: 'রহিম', age: 41, visits: NumberLong(7)}"),
                  canonical("{_id: 3, name: 'Dana', age: 25, city: 'Sylhet'}"),
                  canonical("{_id: 4, name: 'Eve', age: 35, at: ISODate('2024-01-15T10:30:00Z')}")},
                 &error);
    if(!error.isEmpty())
        return {QStringLiteral("mongotab: seeding failed: ") + error};

    tab->reloadDatabases();
    tab->waitForIdle();
    tab->openCollection(db, QStringLiteral("people"));
    tab->waitForIdle();
    tab->reloadDatabases(); /* the open database's collections listed too */
    tab->waitForIdle();
    for(const QString &line : tab->treeForTest())
        if(line.contains(QStringLiteral("openyog_test_mongo_ui")) ||
           line.contains(QStringLiteral("people")))
            out << QStringLiteral("mongotab tree: ") + line.trimmed();
    out << QStringLiteral("mongotab page: ") + tab->pageText();
    for(const QString &line : tab->gridForTest())
        out << QStringLiteral("mongotab grid: ") + line;

    tab->setFind(QStringLiteral("{age: {$gte: 30}}"), QStringLiteral("{name: 1, age: 1}"),
                 QStringLiteral("{age: -1}"), 2);
    tab->run();
    tab->waitForIdle();
    out << QStringLiteral("mongotab filtered: %1 / %2")
               .arg(tab->gridForTest().mid(1).join(QStringLiteral(" ; ")), tab->pageText());
    tab->page(1);
    tab->waitForIdle();
    out << QStringLiteral("mongotab next page: %1 / %2")
               .arg(tab->gridForTest().mid(1).join(QStringLiteral(" ; ")), tab->pageText());

    tab->setFind(QStringLiteral("{_id: 2}"));
    tab->run();
    tab->waitForIdle();
    const bool saved = tab->saveDocumentForTest(
        0, QStringLiteral("{_id: 2, name: 'রহিম', age: 42, visits: NumberLong(8)}"), &error);
    out << QStringLiteral("mongotab edit: %1 %2")
               .arg(saved ? QStringLiteral("saved") : QStringLiteral("FAILED ") + error,
                    tab->jsonForTest().simplified());
    error.clear();
    const bool idChanged =
        tab->saveDocumentForTest(0, QStringLiteral("{_id: 99, name: 'x'}"), &error);
    out << QStringLiteral("mongotab changed _id: %1")
               .arg(idChanged ? QStringLiteral("SAVED (wrong)") : error);

    tab->setFind(QString(), QString(), QStringLiteral("{_id: 1}"), 50);
    tab->run();
    tab->waitForIdle();
    error.clear();
    const bool inserted =
        tab->insertDocumentForTest(QStringLiteral("{name: 'Zed', age: 50}"), &error);
    out << QStringLiteral("mongotab insert: %1, %2")
               .arg(inserted ? QStringLiteral("inserted") : QStringLiteral("FAILED ") + error,
                    tab->pageText());
    const int rows = int(tab->gridForTest().size()) - 1;
    error.clear();
    const bool deleted = tab->deleteRowsForTest({rows - 1}, &error); /* Zed, sorted last */
    out << QStringLiteral("mongotab delete: %1, %2")
               .arg(deleted ? QStringLiteral("deleted") : QStringLiteral("FAILED ") + error,
                    tab->pageText());

    tab->setMode(MongoTab::Mode::Aggregate,
                 QStringLiteral("[{$match: {city: {$exists: true}}}, "
                                "{$group: {_id: '$city', n: {$sum: 1}}}, {$sort: {_id: 1}}]"));
    tab->run();
    tab->waitForIdle();
    out << QStringLiteral("mongotab aggregate: ") + tab->gridForTest().join(QStringLiteral(" ; "));
    tab->setMode(MongoTab::Mode::Command, QStringLiteral("{count: 'people'}"));
    tab->run();
    tab->waitForIdle();
    out << QStringLiteral("mongotab command: ") + tab->gridForTest().join(QStringLiteral(" ; "));
    tab->setMode(MongoTab::Mode::Find);
    tab->setFind(QStringLiteral("{age: {$gt: }}"));
    tab->run();
    tab->waitForIdle();
    out << QStringLiteral("mongotab bad filter: ") + tab->statusText();

    /* back to the whole collection for the screenshot */
    tab->setFind(QString());
    tab->run();
    tab->waitForIdle();
    seed->dropDatabase(db, nullptr);
    return out;
}

QStringList MainWindow::selftestImportExternal(const QString &spec)
{
    auto *tab = currentTab();
    std::unique_ptr<ExternalDataDialog> dialog(makeImportDialog());
    if(!tab || !dialog)
        return {QStringLiteral("importexternal: not connected")};
    QStringList p = spec.split(QLatin1Char(':'));
    /* a Windows path's drive letter isn't a field of its own */
    for(int i = 0; i + 1 < p.size(); ++i)
        if(p[i].size() == 1 && p[i][0].isLetter() &&
           (p[i + 1].startsWith(QLatin1Char('\\')) || p[i + 1].startsWith(QLatin1Char('/'))))
            p[i] += QLatin1Char(':') + p.takeAt(i + 1);
    /* :shot=FILE: the columns page as FILE, the import page as FILE-run.png */
    QString shot;
    if(p.last().startsWith(QStringLiteral("shot=")))
        shot = p.takeLast().mid(5);
    QString table;
    if(p.value(0) == QStringLiteral("csv") && p.size() == 3) {
        dialog->setCsvSourceForTest(p[1]);
        table = p[2];
    } else if(p.value(0) == QStringLiteral("xlsx") && p.size() == 4) {
        dialog->setExcelSourceForTest(p[1], p[2]);
        table = p[3];
    } else if(p.value(0) == QStringLiteral("db") && p.size() == 3) {
        dialog->setDatabaseSourceForTest(0, p[1].section(QLatin1Char('.'), 0, 0),
                                         p[1].section(QLatin1Char('.'), 1));
        table = p[2];
    } else {
        return {QStringLiteral("importexternal: bad spec %1").arg(spec)};
    }
    const QString db = tab->defaultDb().isEmpty() ? tab->databases().value(0) : tab->defaultDb();
    dialog->setNewTableForTest(db, table);
    dialog->show();
    QStringList out;
    for(const QString &line : dialog->mappingForTest())
        out << QStringLiteral("importexternal column: ") + line;
    if(!shot.isEmpty())
        dialog->grab().save(shot);
    out << QStringLiteral("importexternal: ") + dialog->importForTest();
    if(!shot.isEmpty())
        dialog->grab().save(
            QString(shot).replace(QStringLiteral(".png"), QStringLiteral("-run.png")));

    /* what's in the table now, binary values as hex */
    IDbConnection *c = tab->connectionFor({}, nullptr);
    const QString qualified = c->qualify(db, table);
    int rows = 0;
    QString error;
    c->streamQuery(
        QStringLiteral("SELECT * FROM %1 ORDER BY 1").arg(qualified), &error, {},
        [&](const QVector<QByteArray> &fields, const QVector<bool> &isNull) {
            QStringList cells;
            for(int i = 0; i < fields.size(); ++i) {
                if(isNull[i]) {
                    cells << QStringLiteral("NULL");
                    continue;
                }
                QStringDecoder utf8(QStringConverter::Utf8);
                QString text = utf8(fields[i]);
                const bool binary =
                    utf8.hasError() || std::any_of(text.cbegin(), text.cend(), [](QChar ch) {
                        return ch.unicode() < 0x20 && ch != QLatin1Char('\r') &&
                               ch != QLatin1Char('\n');
                    });
                cells << (binary ? QStringLiteral("0x") + QString::fromLatin1(fields[i].toHex())
                                 : text.replace(QLatin1Char('\r'), QStringLiteral("\\r"))
                                       .replace(QLatin1Char('\n'), QStringLiteral("\\n")));
            }
            out << QStringLiteral("importexternal row: ") + cells.join(QStringLiteral(" | "));
            ++rows;
            return true;
        });
    out << QStringLiteral("importexternal rows: %1%2").arg(rows).arg(error);
    c->query(QStringLiteral("DROP TABLE %1").arg(qualified), nullptr, nullptr);
    tab->refreshBrowser();
    return out;
}

QStringList MainWindow::selftestMongoOpen(const QString &spec)
{
    auto *tab = qobject_cast<MongoTab *>(m_tabs->currentWidget());
    if(!tab)
        return {QStringLiteral("mongoopen: no MongoDB tab")};
    tab->waitForIdle();
    tab->openCollection(spec.section(QLatin1Char('.'), 0, 0), spec.section(QLatin1Char('.'), 1));
    tab->waitForIdle();
    tab->reloadDatabases(); /* the open database expanded */
    tab->waitForIdle();
    return {QStringLiteral("mongoopen: ") + tab->pageText()};
}

QStringList MainWindow::selftestQueryBuilder(const QString &spec)
{
    auto *tab = currentTab();
    QueryBuilder *builder = tab ? tab->openQueryBuilder() : nullptr;
    if(!builder)
        return {QStringLiteral("querybuilder: not opened")};
    QStringList out;
    QString error;
    const bool saved = spec.startsWith(QLatin1Char('@'));
    if(saved) {
        QStringList leftOut;
        if(!builder->openFile(spec.mid(1), &error, &leftOut))
            return {QStringLiteral("querybuilder open failed: ") + error};
        for(const QString &what : std::as_const(leftOut))
            out << QStringLiteral("querybuilder left out: ") + what;
    } else {
        for(const QString &part : spec.split(QLatin1Char('+'), Qt::SkipEmptyParts))
            builder->addTable(
                part.section(QLatin1Char(':'), 0, 0),
                part.section(QLatin1Char(':'), 1).split(QLatin1Char(','), Qt::SkipEmptyParts));
    }
    const QString sql = builder->sql();
    out << QStringLiteral("querybuilder sql:");
    out << sql.split(QLatin1Char('\n'));
    const int rows = tab->queryRowCountForTest(sql, &error);
    out << (rows < 0 ? QStringLiteral("querybuilder run failed: ") + error
                     : QStringLiteral("querybuilder rows: %1").arg(rows));
    if(saved) {
        /* saved again and opened in a builder of its own: the same query */
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("again.oyqb"));
        QueryBuilder *again = tab->openQueryBuilder();
        const bool same = builder->saveFile(file, &error) && again &&
                          again->openFile(file, &error) && again->sql() == sql;
        out << QStringLiteral("querybuilder saved and reopened: %1")
                   .arg(same ? QStringLiteral("same sql") : QStringLiteral("DIFFERS ") + error);
    }
    return out;
}

QStringList MainWindow::selftestSchemaSync(const QString &spec)
{
    auto *tab = currentTab();
    const QStringList parts = spec.split(QLatin1Char(':'));
    if(!tab || parts.size() < 2)
        return {QStringLiteral("schemasync: need a connection and SOURCE:TARGET")};
    const auto changeLines = [](const QList<SchemaSync::Change> &changes) {
        QStringList out;
        for(const SchemaSync::Change &c : changes)
            out << QStringLiteral("schemasync change %1: %2%3%4")
                       .arg(c.table, SchemaSync::describe(c),
                            c.pickedByDefault ? QString() : QStringLiteral(" (not picked)"),
                            c.note.isEmpty() ? QString()
                                             : QStringLiteral(" (not done: %1)").arg(c.note));
        return out;
    };
    const SchemaSync::Schema source = tab->syncSchema(parts[0]);
    SchemaSync::Schema target = tab->syncSchema(parts[1]);
    const QList<SchemaSync::Change> changes = SchemaSync::compare(source, target);
    QStringList out{QStringLiteral("schemasync: %1 changes").arg(changes.size())};
    out << changeLines(changes);

    /* what the dialog shows: picked by default, plus what can't be done */
    SchemaSyncDialog dialog({{tab->title(),
                              tab->driverType(),
                              [tab] { return tab->databases(); },
                              [tab] { return tab->defaultDb(); },
                              [tab](const QString &db) { return tab->syncSchema(db); },
                              {}}},
                            this);
    dialog.compareForTest(0, parts[0], 0, parts[1]);
    for(const QString &part : parts.mid(2))
        if(part.startsWith(QStringLiteral("shot="))) {
            dialog.show();
            QApplication::processEvents();
            dialog.grab().save(part.mid(5));
        }
    out << QStringLiteral("schemasync script:") << dialog.scriptText().split(QLatin1Char('\n'));

    if(parts.value(2) == QStringLiteral("apply")) {
        /* every change it can make, the unpicked ones too */
        const QString all = SchemaSync::script(changes, source, target);
        int failed = 0;
        for(const QString &statement : splitStatements(all, tab->driverType())) {
            QString error;
            if(tab->queryRowCountForTest(statement, &error) < 0) {
                ++failed;
                out << QStringLiteral("schemasync apply failed: %1 -- %2")
                           .arg(statement.simplified(), error);
            }
        }
        target = tab->syncSchema(parts[1]);
        const QList<SchemaSync::Change> left = SchemaSync::compare(source, target);
        out << QStringLiteral("schemasync after apply: %1 changes, %2 failed statements")
                   .arg(left.size())
                   .arg(failed);
        out << changeLines(left);
    }
    return out;
}

QStringList MainWindow::selftestResults() const
{
    const ConnectionTab *tab = currentTab();
    return tab ? tab->resultsForTest() : QStringList();
}

QStringList MainWindow::selftestDataSync(const QString &spec)
{
    auto *tab = currentTab();
    const QStringList parts = spec.split(QLatin1Char(':'));
    QString error;
    IDbConnection *conn = tab ? tab->connectionFor(QString(), &error) : nullptr;
    if(!conn || parts.size() < 2)
        return {QStringLiteral("datasync: need a connection and SOURCE:TARGET")};
    const QString source = parts[0], target = parts[1];
    const auto compareAll = [&](QStringList *out) {
        qint64 differences = 0;
        for(const QString &table : DataSync::commonTables(conn, source, conn, target)) {
            const DataSync::TableResult r =
                DataSync::compareTable(conn, source, conn, target, table);
            differences += r.inserts + r.updates + r.deletes;
            if(out)
                *out << (r.skipped.isEmpty()
                             ? QStringLiteral(
                                   "datasync %1: %2 insert, %3 update, %4 delete, %5 same")
                                   .arg(table)
                                   .arg(r.inserts)
                                   .arg(r.updates)
                                   .arg(r.deletes)
                                   .arg(r.same)
                             : QStringLiteral("datasync %1: skipped (%2)").arg(table, r.skipped));
        }
        return differences;
    };
    QStringList out;
    out << QStringLiteral("datasync: %1 row differences").arg(compareAll(&out));
    for(const QString &part : parts.mid(2))
        if(part.startsWith(QStringLiteral("shot="))) {
            /* the window itself: its worker, the drivers, what it lists */
            DataSyncDialog dialog({{tab->title(),
                                    tab->driverType(),
                                    [tab] { return tab->databases(); },
                                    [tab] { return tab->defaultDb(); },
                                    [params = tab->params()](const QString &) { return params; },
                                    {}}},
                                  this);
            dialog.show();
            dialog.compareForTest(0, source, 0, target);
            for(const QString &line : dialog.shownForTest())
                out << QStringLiteral("datasync dialog: ") + line;
            dialog.grab().save(part.mid(5));
        }
    if(parts.value(2) == QStringLiteral("apply")) {
        QList<DataSync::Pick> picks;
        for(const QString &table : DataSync::commonTables(conn, source, conn, target))
            picks.append({table, true, true, true});
        QByteArray script;
        if(!DataSync::writeScript(
               conn, source, conn, target, picks, [&](const QByteArray &part) { script += part; },
               {}, &error))
            return out << QStringLiteral("datasync script failed: ") + error;
        int failed = 0;
        for(const QString &statement :
            splitStatements(QString::fromUtf8(script), tab->driverType())) {
            if(tab->queryRowCountForTest(statement, &error) < 0) {
                ++failed;
                out << QStringLiteral("datasync apply failed: %1 -- %2")
                           .arg(statement.simplified().left(200), error);
            }
        }
        out << QStringLiteral("datasync after apply: %1 differences, %2 failed statements")
                   .arg(compareAll(nullptr))
                   .arg(failed);
    }
    return out;
}
