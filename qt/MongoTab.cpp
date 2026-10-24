/* OpenYog — a MongoDB connection's tab; see MongoTab.h. */
#include "MongoTab.h"
#include "Icons.h"
#include "db/MongoClient.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QThreadPool>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

enum Role { RoleKind = Qt::UserRole, RoleDb, RoleLoaded };
enum Kind { KDatabase, KCollection };

MongoJson::Value parsed(const QByteArray &canonical)
{
    MongoJson::Value v;
    MongoJson::parse(QString::fromUtf8(canonical), &v);
    return v;
}

bool numeric(const MongoJson::Value &v)
{
    if(v.type == MongoJson::Value::Number)
        return true;
    if(!v.isObject() || v.members.size() != 1)
        return false;
    const QString &k = v.members.front().first;
    return k == QStringLiteral("$numberInt") || k == QStringLiteral("$numberLong") ||
           k == QStringLiteral("$numberDouble") || k == QStringLiteral("$numberDecimal");
}

/* a document's _id as a filter on it */
QByteArray idFilter(const MongoJson::Value &id)
{
    return MongoJson::toCanonical(MongoJson::Value::object({{QStringLiteral("_id"), id}}));
}

QFont fixedFont()
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

/* a document as text in a dialog: edited (validated as a document before it
 * closes) or only shown */
bool documentDialog(QWidget *parent, const QString &title, QString *text, bool readOnly,
                    const QString &problem = {})
{
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.resize(640, 520);
    auto *editor = new QPlainTextEdit(&dlg);
    editor->setFont(fixedFont());
    editor->setPlainText(*text);
    editor->setReadOnly(readOnly);
    editor->setTabChangesFocus(false);
    auto *hint = new QLabel(&dlg);
    hint->setWordWrap(true);
    const auto showProblem = [hint](const QString &p) {
        hint->setText(p.isEmpty() ? QStringLiteral("Shell notation or JSON: {name: 'x', n: "
                                                   "NumberLong(5), at: ISODate('2024-01-15')}")
                                  : p);
        hint->setStyleSheet(p.isEmpty() ? QString() : QStringLiteral("color: #c0392b"));
    };
    showProblem(problem);
    auto *buttons = new QDialogButtonBox(
        readOnly ? QDialogButtonBox::Close : QDialogButtonBox::Save | QDialogButtonBox::Cancel,
        &dlg);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        MongoJson::Value v;
        QString error;
        if(!MongoJson::parseDocument(editor->toPlainText(), &v, &error)) {
            showProblem(error);
            return;
        }
        dlg.accept();
    });
    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(editor, 1);
    if(!readOnly)
        lay->addWidget(hint);
    lay->addWidget(buttons);
    if(dlg.exec() != QDialog::Accepted)
        return false;
    *text = editor->toPlainText();
    return true;
}

} // namespace

MongoTab::MongoTab(const ConnectionParams &params, std::shared_ptr<MongoClient> client,
                   QWidget *parent)
    : QWidget(parent), m_params(params), m_client(std::move(client))
{
    /* ---- toolbar ---------------------------------------------------- */
    m_toolbar = new QToolBar(this);
    m_toolbar->setIconSize(QSize(16, 16));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_toolbar
        ->addAction(Icons::get(QStringLiteral("refresh_16.ico")), QStringLiteral("Refresh"), this,
                    &MongoTab::reloadDatabases)
        ->setToolTip(QStringLiteral("Read the databases and collections again"));
    m_toolbar->addSeparator();
    m_insertAction =
        m_toolbar->addAction(Icons::get(QStringLiteral("result_insert.ico")),
                             QStringLiteral("Insert"), this, [this] { insertDocument(); });
    m_insertAction->setToolTip(QStringLiteral("Insert a document into the collection"));
    m_editAction = m_toolbar->addAction(Icons::get(QStringLiteral("insertupdate.ICO")),
                                        QStringLiteral("Edit"), this, [this] {
                                            const QList<int> rows = selectedRows();
                                            if(!rows.isEmpty())
                                                editDocument(rows.first());
                                        });
    m_editAction->setToolTip(QStringLiteral("Edit the selected document (or double-click it)"));
    m_deleteAction =
        m_toolbar->addAction(Icons::get(QStringLiteral("result_delete.ico")),
                             QStringLiteral("Delete"), this, &MongoTab::deleteSelected);
    m_deleteAction->setToolTip(QStringLiteral("Delete the selected documents"));
    m_toolbar->addSeparator();
    m_indexAction =
        m_toolbar->addAction(Icons::get(QStringLiteral("manage_index_16.ico")),
                             QStringLiteral("Indexes…"), this, &MongoTab::manageIndexes);
    m_importAction =
        m_toolbar->addAction(Icons::get(QStringLiteral("import_data.ico")),
                             QStringLiteral("Import…"), this, &MongoTab::importCollection);
    m_importAction->setToolTip(
        QStringLiteral("Add documents from a file: one per line, or a JSON array"));
    m_exportAction =
        m_toolbar->addAction(Icons::get(QStringLiteral("export_data_16.ico")),
                             QStringLiteral("Export…"), this, &MongoTab::exportCollection);
    m_exportAction->setToolTip(
        QStringLiteral("Write the documents the filter matches to a file, one per line"));

    /* ---- databases and collections ---------------------------------- */
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, &MongoTab::showTreeMenu);
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if(item->data(0, RoleKind).toInt() == KDatabase && !item->data(0, RoleLoaded).toBool())
            loadCollections(item);
    });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if(item->data(0, RoleKind).toInt() == KCollection)
            openCollection(item->data(0, RoleDb).toString(), item->text(0));
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        /* a database chosen: where a command runs */
        if(item && item->data(0, RoleKind).toInt() == KDatabase && item->text(0) != m_db) {
            m_db = item->text(0);
            m_collection.clear();
            m_where->setText(m_db);
            updateActions();
        }
    });

    /* ---- the query --------------------------------------------------- */
    m_where = new QLabel(QStringLiteral("Open a collection: double-click it on the left"), this);
    QFont bold = m_where->font();
    bold.setBold(true);
    m_where->setFont(bold);
    m_modeBox = new QComboBox(this);
    m_modeBox->addItems(
        {QStringLiteral("Find"), QStringLiteral("Aggregate"), QStringLiteral("Command")});
    connect(m_modeBox, &QComboBox::currentIndexChanged, this, &MongoTab::modeChanged);
    m_runButton =
        new QPushButton(Icons::get(QStringLiteral("execute_16.ico")), QStringLiteral("&Run"), this);
    m_runButton->setToolTip(QStringLiteral("Run it (Ctrl+Enter in the editors)"));
    connect(m_runButton, &QPushButton::clicked, this, [this] {
        m_skip = 0;
        run();
    });
    auto *top = new QHBoxLayout;
    top->addWidget(m_where, 1);
    top->addWidget(new QLabel(QStringLiteral("Mode"), this));
    top->addWidget(m_modeBox);
    top->addWidget(m_runButton);

    const auto lineEdit = [this](const QString &placeholder) {
        auto *e = new QLineEdit(this);
        e->setFont(fixedFont());
        e->setPlaceholderText(placeholder);
        e->setClearButtonEnabled(true);
        connect(e, &QLineEdit::returnPressed, this, [this] {
            m_skip = 0;
            run();
        });
        return e;
    };
    m_filter = lineEdit(QStringLiteral("{} — e.g. {age: {$gte: 30}, name: /^a/i}"));
    m_projection = lineEdit(QStringLiteral("all fields — e.g. {name: 1, _id: 0}"));
    m_sort = lineEdit(QStringLiteral("natural order — e.g. {age: -1}"));
    m_limit = new QSpinBox(this);
    m_limit->setRange(1, 10000);
    m_limit->setValue(50);
    m_limit->setToolTip(QStringLiteral("Documents per page"));
    auto *findPage = new QWidget(this);
    auto *findForm = new QGridLayout(findPage);
    findForm->setContentsMargins(0, 0, 0, 0);
    const auto label = [this](const QString &text, QWidget *buddy) {
        auto *l = new QLabel(text, this);
        l->setBuddy(buddy);
        return l;
    };
    findForm->addWidget(label(QStringLiteral("&Filter"), m_filter), 0, 0);
    findForm->addWidget(m_filter, 0, 1, 1, 3);
    findForm->addWidget(label(QStringLiteral("Pro&jection"), m_projection), 1, 0);
    findForm->addWidget(m_projection, 1, 1);
    findForm->addWidget(label(QStringLiteral("&Sort"), m_sort), 1, 2);
    findForm->addWidget(m_sort, 1, 3);
    findForm->addWidget(label(QStringLiteral("Pa&ge"), m_limit), 0, 4);
    findForm->addWidget(m_limit, 0, 5);
    findForm->setColumnStretch(1, 1);
    findForm->setColumnStretch(3, 1);

    const auto textEdit = [this](const QString &placeholder) {
        auto *e = new QPlainTextEdit(this);
        e->setFont(fixedFont());
        e->setPlaceholderText(placeholder);
        e->setMaximumHeight(110);
        auto *runShortcut = new QAction(e);
        runShortcut->setShortcuts(
            {QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)});
        runShortcut->setShortcutContext(Qt::WidgetShortcut);
        connect(runShortcut, &QAction::triggered, this, &MongoTab::run);
        e->addAction(runShortcut);
        return e;
    };
    m_pipeline = textEdit(
        QStringLiteral("[{$match: {age: {$gte: 30}}}, {$group: {_id: '$city', n: {$sum: 1}}}]"));
    m_command = textEdit(
        QStringLiteral("a command for the database: {collStats: 'people'}, {dbStats: 1} …"));
    m_queryStack = new QStackedWidget(this);
    m_queryStack->addWidget(findPage);
    m_queryStack->addWidget(m_pipeline);
    m_queryStack->addWidget(m_command);
    m_queryStack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    /* ---- the documents ------------------------------------------------ */
    m_grid = new QTableWidget(this);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->horizontalHeader()->setDefaultSectionSize(140);
    m_grid->verticalHeader()->setDefaultSectionSize(m_grid->fontMetrics().height() + 6);
    connect(m_grid, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { editDocument(row); });
    connect(m_grid, &QWidget::customContextMenuRequested, this, &MongoTab::showGridMenu);
    connect(m_grid, &QTableWidget::itemSelectionChanged, this, &MongoTab::updateActions);
    m_json = new QPlainTextEdit(this);
    m_json->setReadOnly(true);
    m_json->setFont(fixedFont());
    m_results = new QTabWidget(this);
    m_results->addTab(m_grid, Icons::get(QStringLiteral("grid_view.ico")),
                      QStringLiteral("Documents"));
    m_results->addTab(m_json, Icons::get(QStringLiteral("text_View.ico")), QStringLiteral("Text"));

    m_prev = new QPushButton(Icons::get(QStringLiteral("previousbutton.ico")), QString(), this);
    m_prev->setToolTip(QStringLiteral("Previous page"));
    connect(m_prev, &QPushButton::clicked, this, [this] { page(-1); });
    m_next = new QPushButton(Icons::get(QStringLiteral("nextbutton.ico")), QString(), this);
    m_next->setToolTip(QStringLiteral("Next page"));
    connect(m_next, &QPushButton::clicked, this, [this] { page(1); });
    m_pageLabel = new QLabel(this);
    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_prev);
    bottom->addWidget(m_pageLabel);
    bottom->addWidget(m_next);
    bottom->addSpacing(12);
    bottom->addWidget(m_status, 1);

    auto *right = new QWidget(this);
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(4, 4, 4, 4);
    rightLay->addLayout(top);
    rightLay->addWidget(m_queryStack);
    rightLay->addWidget(m_results, 1);
    rightLay->addLayout(bottom);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(m_tree);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    split->setSizes({220, 800});

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_toolbar);
    lay->addWidget(split, 1);

    setStatus(QStringLiteral("Connected to %1, MongoDB %2")
                  .arg(m_client->address(), m_client->serverVersion()));
    modeChanged();
    updateActions();
    reloadDatabases();
}

MongoTab::~MongoTab() = default;

void MongoTab::async(const QString &busy, Work work, std::function<void(const Result &)> done)
{
    ++m_pending;
    if(!busy.isEmpty())
        setStatus(busy);
    m_runButton->setEnabled(false);
    QPointer<MongoTab> self(this);
    std::shared_ptr<MongoClient> client = m_client;
    QThreadPool::globalInstance()->start(
        [self, client, work = std::move(work), done = std::move(done)] {
            const Result r = work(*client);
            QMetaObject::invokeMethod(
                qApp,
                [self, r, done] {
                    if(!self)
                        return; /* the tab closed meanwhile */
                    --self->m_pending;
                    self->m_runButton->setEnabled(self->m_pending == 0);
                    if(!r.error.isEmpty())
                        self->setStatus(r.error, true);
                    else if(!r.message.isEmpty())
                        self->setStatus(r.message);
                    done(r);
                    self->updateActions();
                },
                Qt::QueuedConnection);
        });
}

bool MongoTab::waitForIdle(int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while(m_pending > 0 && t.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(5);
    }
    QApplication::processEvents();
    return m_pending == 0;
}

void MongoTab::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? QStringLiteral("color: #c0392b") : QString());
    m_status->setToolTip(text);
    emit statusMessage(text);
}

QString MongoTab::statusText() const
{
    return m_status->text();
}

QString MongoTab::pageText() const
{
    return m_pageLabel->text();
}

/* ---- databases and collections -------------------------------------- */

void MongoTab::reloadDatabases()
{
    QStringList expanded;
    for(int i = 0; i < m_tree->topLevelItemCount(); ++i)
        if(m_tree->topLevelItem(i)->isExpanded())
            expanded << m_tree->topLevelItem(i)->text(0);
    if(!m_db.isEmpty())
        expanded << m_db;
    else if(!m_params.database.isEmpty())
        expanded << m_params.database;
    async(
        QStringLiteral("Reading the databases…"),
        [](MongoClient &c) {
            Result r;
            r.names = c.databases(&r.error);
            return r;
        },
        [this, expanded](const Result &r) {
            QStringList names = r.names;
            /* a user who may not list databases still has the one named */
            if(names.isEmpty() && !m_params.database.isEmpty())
                names << m_params.database;
            m_tree->clear();
            for(const QString &name : std::as_const(names)) {
                auto *item = new QTreeWidgetItem(m_tree, {name});
                item->setIcon(0, Icons::get(QStringLiteral("database.ico")));
                item->setData(0, RoleKind, KDatabase);
                item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
                if(expanded.contains(name))
                    item->setExpanded(true); /* loads its collections */
            }
            if(r.error.isEmpty())
                setStatus(QStringLiteral("%1 databases").arg(names.size()));
        });
}

void MongoTab::loadCollections(QTreeWidgetItem *dbItem)
{
    const QString db = dbItem->text(0);
    dbItem->setData(0, RoleLoaded, true);
    async(
        QStringLiteral("Reading the collections of %1…").arg(db),
        [db](MongoClient &c) {
            Result r;
            for(const MongoClient::Collection &col : c.collections(db, &r.error)) {
                r.names << col.name;
                r.types << col.type;
            }
            return r;
        },
        [this, db](const Result &r) {
            QTreeWidgetItem *dbItem = nullptr;
            for(int i = 0; i < m_tree->topLevelItemCount() && !dbItem; ++i)
                if(m_tree->topLevelItem(i)->text(0) == db)
                    dbItem = m_tree->topLevelItem(i);
            if(!dbItem)
                return; /* refreshed away meanwhile */
            qDeleteAll(dbItem->takeChildren());
            for(int i = 0; i < r.names.size(); ++i) {
                auto *item = new QTreeWidgetItem(dbItem, {r.names[i]});
                const bool view = r.types.value(i) == QStringLiteral("view");
                item->setIcon(0, Icons::get(view ? QStringLiteral("alterview.ico")
                                                 : QStringLiteral("table.ico")));
                item->setData(0, RoleKind, KCollection);
                item->setData(0, RoleDb, db);
                item->setToolTip(0, r.types.value(i));
                if(db == m_db && r.names[i] == m_collection)
                    m_tree->setCurrentItem(item);
            }
            dbItem->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
            if(r.error.isEmpty())
                setStatus(QStringLiteral("%1: %2 collections").arg(db).arg(r.names.size()));
        });
}

QStringList MongoTab::treeForTest() const
{
    QStringList out;
    for(int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *db = m_tree->topLevelItem(i);
        out << db->text(0);
        for(int j = 0; j < db->childCount(); ++j)
            out << QStringLiteral("  %1 (%2)").arg(db->child(j)->text(0), db->child(j)->toolTip(0));
    }
    return out;
}

void MongoTab::showTreeMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    QMenu menu(this);
    if(!item) {
        menu.addAction(Icons::get(QStringLiteral("database.ico")),
                       QStringLiteral("Create &Database…"), this, [this] {
                           bool ok = false;
                           const QString db =
                               QInputDialog::getText(this, QStringLiteral("Create Database"),
                                                     QStringLiteral("Database name:"),
                                                     QLineEdit::Normal, {}, &ok)
                                   .trimmed();
                           if(ok && !db.isEmpty())
                               createCollection(db); /* a database is there once it has one */
                       });
    } else if(item->data(0, RoleKind).toInt() == KDatabase) {
        const QString db = item->text(0);
        menu.addAction(Icons::get(QStringLiteral("createtableindata.ico")),
                       QStringLiteral("Create &Collection…"), this,
                       [this, db] { createCollection(db); });
        menu.addAction(Icons::get(QStringLiteral("refresh_16.ico")), QStringLiteral("Re&fresh"),
                       this, [this, item] { loadCollections(item); });
        menu.addSeparator();
        menu.addAction(Icons::get(QStringLiteral("dropdatabase.ico")),
                       QStringLiteral("D&rop Database…"), this, [this, db] { dropDatabase(db); });
    } else {
        const QString db = item->data(0, RoleDb).toString(), coll = item->text(0);
        menu.addAction(Icons::get(QStringLiteral("viewdata.ico")), QStringLiteral("&Open"), this,
                       [this, db, coll] { openCollection(db, coll); });
        menu.addAction(Icons::get(QStringLiteral("manage_index_16.ico")),
                       QStringLiteral("&Indexes…"), this, [this, db, coll] {
                           openCollection(db, coll);
                           manageIndexes();
                       });
        menu.addAction(Icons::get(QStringLiteral("import_data.ico")), QStringLiteral("I&mport…"),
                       this, [this, db, coll] {
                           openCollection(db, coll);
                           importCollection();
                       });
        menu.addAction(Icons::get(QStringLiteral("export_data_16.ico")), QStringLiteral("&Export…"),
                       this, [this, db, coll] {
                           openCollection(db, coll);
                           exportCollection();
                       });
        menu.addSeparator();
        menu.addAction(Icons::get(QStringLiteral("drop_table.ico")),
                       QStringLiteral("D&rop Collection…"), this,
                       [this, db, coll] { dropCollection(db, coll); });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void MongoTab::createCollection(const QString &db)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Create Collection"),
                                               QStringLiteral("Collection in %1:").arg(db),
                                               QLineEdit::Normal, {}, &ok)
                             .trimmed();
    if(!ok || name.isEmpty())
        return;
    async(
        {},
        [db, name](MongoClient &c) {
            Result r;
            if(c.createCollection(db, name, &r.error))
                r.message = QStringLiteral("Created %1.%2").arg(db, name);
            return r;
        },
        [this, db, name](const Result &r) {
            if(!r.error.isEmpty())
                return;
            m_db = db;
            m_collection = name;
            reloadDatabases();
            openCollection(db, name);
        });
}

void MongoTab::dropCollection(const QString &db, const QString &collection)
{
    if(QMessageBox::warning(
           this, QStringLiteral("Drop Collection"),
           QStringLiteral("Drop %1.%2 with all its documents and indexes?").arg(db, collection),
           QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    async(
        {},
        [db, collection](MongoClient &c) {
            Result r;
            if(c.dropCollection(db, collection, &r.error))
                r.message = QStringLiteral("Dropped %1.%2").arg(db, collection);
            return r;
        },
        [this, db, collection](const Result &) {
            if(m_db == db && m_collection == collection) {
                m_collection.clear();
                m_where->setText(db);
                showDocuments({}, false);
            }
            reloadDatabases();
        });
}

void MongoTab::dropDatabase(const QString &db)
{
    if(QMessageBox::warning(
           this, QStringLiteral("Drop Database"),
           QStringLiteral("Drop the database %1 with every collection in it?").arg(db),
           QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    async(
        {},
        [db](MongoClient &c) {
            Result r;
            if(c.dropDatabase(db, &r.error))
                r.message = QStringLiteral("Dropped the database %1").arg(db);
            return r;
        },
        [this, db](const Result &) {
            if(m_db == db) {
                m_db.clear();
                m_collection.clear();
                m_where->setText(QString());
                showDocuments({}, false);
            }
            reloadDatabases();
        });
}

/* ---- the query -------------------------------------------------------- */

void MongoTab::openCollection(const QString &db, const QString &collection)
{
    if(db == m_db && collection == m_collection && m_pending == 0 && !m_docs.empty())
        return;
    m_db = db;
    m_collection = collection;
    m_skip = 0;
    m_where->setText(QStringLiteral("%1 — %2").arg(collection, db));
    if(m_mode == Mode::Command)
        setMode(Mode::Find);
    run();
}

void MongoTab::setFind(const QString &filter, const QString &projection, const QString &sort,
                       int limit)
{
    m_filter->setText(filter);
    m_projection->setText(projection);
    m_sort->setText(sort);
    m_limit->setValue(limit);
    m_skip = 0;
}

void MongoTab::setMode(Mode mode, const QString &text)
{
    m_modeBox->setCurrentIndex(int(mode));
    if(mode == Mode::Aggregate && !text.isNull())
        m_pipeline->setPlainText(text);
    if(mode == Mode::Command && !text.isNull())
        m_command->setPlainText(text);
}

void MongoTab::modeChanged()
{
    m_mode = Mode(m_modeBox->currentIndex());
    m_queryStack->setCurrentIndex(int(m_mode));
    /* only as tall as the page showing: the documents get the rest */
    m_queryStack->setFixedHeight(m_queryStack->currentWidget()->sizeHint().height());
    updatePaging();
}

void MongoTab::page(int direction)
{
    m_skip = std::max<qint64>(0, m_skip + direction * m_limit->value());
    run();
}

void MongoTab::run()
{
    const QString db = m_db, coll = m_collection;
    if(db.isEmpty() || (m_mode != Mode::Command && coll.isEmpty())) {
        setStatus(m_mode == Mode::Command ? QStringLiteral("Choose a database on the left first.")
                                          : QStringLiteral("Open a collection first: "
                                                           "double-click it on the left."),
                  true);
        return;
    }
    QString error;
    if(m_mode == Mode::Find) {
        MongoJson::Value filter, projection, sort;
        const auto read = [&](QLineEdit *e, MongoJson::Value *v, const QString &what) {
            if(MongoJson::parseDocument(e->text(), v, &error))
                return true;
            error = QStringLiteral("%1: %2").arg(what, error);
            e->setFocus();
            return false;
        };
        if(!read(m_filter, &filter, QStringLiteral("Filter")) ||
           !read(m_projection, &projection, QStringLiteral("Projection")) ||
           !read(m_sort, &sort, QStringLiteral("Sort"))) {
            setStatus(error, true);
            return;
        }
        MongoClient::Find f;
        f.filter = MongoJson::toCanonical(filter);
        f.projection = MongoJson::toCanonical(projection);
        f.sort = MongoJson::toCanonical(sort);
        f.skip = m_skip;
        f.limit = m_limit->value();
        async(
            QStringLiteral("Finding…"),
            [db, coll, f](MongoClient &c) {
                Result r;
                if(!c.find(
                       db, coll, f,
                       [&r](const QByteArray &doc) {
                           r.documents << doc;
                           return true;
                       },
                       &r.error))
                    return r;
                r.total = c.count(db, coll, f.filter, nullptr);
                return r;
            },
            [this](const Result &r) {
                m_total = r.total;
                showDocuments(r, true);
            });
        return;
    }
    if(m_mode == Mode::Aggregate) {
        MongoJson::Value pipeline;
        if(!MongoJson::parsePipeline(m_pipeline->toPlainText(), &pipeline, &error)) {
            setStatus(QStringLiteral("Pipeline: ") + error, true);
            return;
        }
        const QByteArray p = MongoJson::toCanonical(pipeline);
        async(
            QStringLiteral("Aggregating…"),
            [db, coll, p](MongoClient &c) {
                Result r;
                c.aggregate(
                    db, coll, p,
                    [&r](const QByteArray &doc) {
                        r.documents << doc;
                        return r.documents.size() < 10000; /* a screenful is plenty */
                    },
                    &r.error);
                return r;
            },
            [this](const Result &r) {
                m_total = -1;
                showDocuments(r, false);
            });
        return;
    }
    MongoJson::Value command;
    if(!MongoJson::parseDocument(m_command->toPlainText(), &command, &error) ||
       command.members.empty()) {
        setStatus(
            QStringLiteral("Command: ") +
                (error.isEmpty() ? QStringLiteral("a command document ({…}) expected") : error),
            true);
        return;
    }
    const QByteArray cmd = MongoJson::toCanonical(command);
    async(
        QStringLiteral("Running the command…"),
        [db, cmd](MongoClient &c) {
            Result r;
            QByteArray reply;
            if(c.command(db, cmd, &reply, &r.error))
                r.documents << reply;
            return r;
        },
        [this](const Result &r) {
            m_total = -1;
            showDocuments(r, false);
        });
}

void MongoTab::showDocuments(const Result &r, bool editable)
{
    m_editable = editable;
    m_docs.clear();
    m_raw = r.documents;
    QStringList columns;
    for(const QByteArray &doc : r.documents) {
        m_docs.push_back(parsed(doc));
        for(const auto &member : m_docs.back().members)
            if(!columns.contains(member.first))
                columns << member.first;
    }
    /* a command's reply or an aggregate's output may have no _id */
    m_grid->clear();
    m_grid->setColumnCount(int(columns.size()));
    m_grid->setHorizontalHeaderLabels(columns);
    m_grid->setRowCount(int(m_docs.size()));
    const QColor missing = palette().color(QPalette::Disabled, QPalette::Text);
    QStringList text;
    for(int row = 0; row < int(m_docs.size()); ++row) {
        const MongoJson::Value &doc = m_docs[size_t(row)];
        for(int col = 0; col < columns.size(); ++col) {
            const MongoJson::Value *v = doc.member(columns[col]);
            auto *item = new QTableWidgetItem;
            if(!v) {
                item->setToolTip(QStringLiteral("(no %1 in this document)").arg(columns[col]));
                item->setForeground(missing);
            } else {
                item->setText(MongoJson::cell(*v));
                if(v->isObject() || v->isArray() || item->text().size() > 60)
                    item->setToolTip(MongoJson::toShell(*v).left(4000));
                if(numeric(*v))
                    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            }
            m_grid->setItem(row, col, item);
        }
        text << MongoJson::toShell(doc);
    }
    /* as wide as what's in them, within reason */
    m_grid->resizeColumnsToContents();
    for(int col = 0; col < m_grid->columnCount(); ++col)
        m_grid->setColumnWidth(col, std::clamp(m_grid->columnWidth(col), 60, 280));
    m_json->setPlainText(text.join(QStringLiteral("\n\n")));
    if(r.error.isEmpty() && r.message.isEmpty())
        setStatus(
            QStringLiteral("%1 document%2").arg(m_docs.size()).arg(m_docs.size() == 1 ? "" : "s"));
    updatePaging();
    updateActions();
}

void MongoTab::updatePaging()
{
    const bool find = m_mode == Mode::Find && !m_collection.isEmpty();
    const qint64 shown = qint64(m_docs.size());
    const QLocale l;
    if(!find) {
        m_pageLabel->setText(shown ? QStringLiteral("%1 results").arg(l.toString(shown))
                                   : QString());
    } else if(shown == 0) {
        m_pageLabel->setText(m_total == 0 ? QStringLiteral("No documents")
                                          : QStringLiteral("No documents on this page"));
    } else {
        m_pageLabel->setText(QStringLiteral("%1–%2 of %3")
                                 .arg(l.toString(m_skip + 1), l.toString(m_skip + shown),
                                      m_total >= 0 ? l.toString(m_total) : QStringLiteral("?")));
    }
    m_prev->setEnabled(find && m_skip > 0);
    m_next->setEnabled(find && shown == m_limit->value() &&
                       (m_total < 0 || m_skip + shown < m_total));
    m_prev->setVisible(find);
    m_next->setVisible(find);
}

void MongoTab::updateActions()
{
    const bool open = !m_collection.isEmpty();
    const bool rows = m_editable && !selectedRows().isEmpty();
    m_insertAction->setEnabled(open);
    m_editAction->setEnabled(rows);
    m_deleteAction->setEnabled(rows);
    m_indexAction->setEnabled(open);
    m_importAction->setEnabled(open);
    m_exportAction->setEnabled(open);
}

QList<int> MongoTab::selectedRows() const
{
    QList<int> rows;
    for(const QModelIndex &i : m_grid->selectionModel()->selectedRows())
        rows << i.row();
    std::sort(rows.begin(), rows.end());
    return rows;
}

QStringList MongoTab::gridForTest() const
{
    QStringList out, header;
    for(int c = 0; c < m_grid->columnCount(); ++c)
        header << m_grid->horizontalHeaderItem(c)->text();
    out << header.join(QStringLiteral(" | "));
    for(int r = 0; r < m_grid->rowCount(); ++r) {
        QStringList cells;
        for(int c = 0; c < m_grid->columnCount(); ++c)
            cells << (m_grid->item(r, c) ? m_grid->item(r, c)->text() : QString());
        out << cells.join(QStringLiteral(" | "));
    }
    return out;
}

QString MongoTab::jsonForTest() const
{
    return m_json->toPlainText();
}

/* ---- documents -------------------------------------------------------- */

void MongoTab::showGridMenu(const QPoint &pos)
{
    const int row = m_grid->rowAt(pos.y());
    QMenu menu(this);
    if(row >= 0 && row < int(m_docs.size())) {
        if(!selectedRows().contains(row))
            m_grid->selectRow(row);
        menu.addAction(Icons::get(QStringLiteral("insertupdate.ICO")),
                       m_editable ? QStringLiteral("&Edit Document…")
                                  : QStringLiteral("&View Document…"),
                       this, [this, row] { editDocument(row); });
        if(m_editable) {
            menu.addAction(Icons::get(QStringLiteral("copy_data_16.ico")),
                           QStringLiteral("D&uplicate Document…"), this, [this, row] {
                               MongoJson::Value doc = m_docs[size_t(row)];
                               doc.members.erase(
                                   std::remove_if(doc.members.begin(), doc.members.end(),
                                                  [](const auto &m) {
                                                      return m.first == QStringLiteral("_id");
                                                  }),
                                   doc.members.end());
                               insertDocument(MongoJson::toShell(doc));
                           });
        }
        menu.addAction(Icons::get(QStringLiteral("Copy.ico")), QStringLiteral("&Copy as Text"),
                       this, [this] {
                           QStringList text;
                           for(int r : selectedRows())
                               text << MongoJson::toShell(m_docs[size_t(r)]);
                           QGuiApplication::clipboard()->setText(text.join(QStringLiteral("\n")));
                       });
        if(m_editable) {
            menu.addSeparator();
            menu.addAction(Icons::get(QStringLiteral("result_delete.ico")),
                           QStringLiteral("&Delete Documents"), this, &MongoTab::deleteSelected);
        }
    } else if(!m_collection.isEmpty()) {
        menu.addAction(Icons::get(QStringLiteral("result_insert.ico")),
                       QStringLiteral("&Insert Document…"), this, [this] { insertDocument(); });
    }
    if(!menu.isEmpty())
        menu.exec(m_grid->viewport()->mapToGlobal(pos));
}

void MongoTab::editDocument(int row)
{
    if(row < 0 || row >= int(m_docs.size()))
        return;
    QString text = MongoJson::toShell(m_docs[size_t(row)]);
    if(!m_editable) {
        documentDialog(this, QStringLiteral("Document"), &text, true);
        return;
    }
    QString problem;
    for(;;) {
        if(!documentDialog(this, QStringLiteral("Edit Document — %1").arg(m_collection), &text,
                           false, problem))
            return;
        QString error;
        /* a server-side refusal reopens the editor with the text kept */
        if(saveDocument(
               row, text,
               [this, row, text](bool ok) {
                   if(!ok &&
                      QMessageBox::question(this, QStringLiteral("Edit Document"),
                                            statusText() + QStringLiteral("\n\nEdit it again?")) ==
                          QMessageBox::Yes) {
                       QString again = text;
                       if(documentDialog(this, QStringLiteral("Edit Document"), &again, false,
                                         statusText()))
                           saveDocument(row, again, {}, nullptr);
                   }
               },
               &error))
            return;
        problem = error;
    }
}

bool MongoTab::saveDocument(int row, const QString &text, const std::function<void(bool)> &then,
                            QString *error)
{
    MongoJson::Value doc;
    QString e;
    if(!MongoJson::parseDocument(text, &doc, &e)) {
        if(error)
            *error = e;
        return false;
    }
    const MongoJson::Value *id = m_docs[size_t(row)].member(QStringLiteral("_id"));
    if(!id) {
        if(error)
            *error = QStringLiteral("this document has no _id to find it by");
        return false;
    }
    const MongoJson::Value *newId = doc.member(QStringLiteral("_id"));
    if(!newId) {
        doc.members.insert(doc.members.begin(), {QStringLiteral("_id"), *id});
    } else if(MongoJson::toCanonical(*newId) != MongoJson::toCanonical(*id)) {
        if(error)
            *error =
                QStringLiteral("the _id can't change: Duplicate makes a copy with another one");
        return false;
    }
    const QByteArray filter = idFilter(*id), replacement = MongoJson::toCanonical(doc);
    const QString db = m_db, coll = m_collection;
    async(
        QStringLiteral("Saving…"),
        [db, coll, filter, replacement](MongoClient &c) {
            Result r;
            qint64 matched = 0;
            if(c.replace(db, coll, filter, replacement, &matched, &r.error))
                r.message = matched ? QStringLiteral("Saved") : QString();
            if(r.error.isEmpty() && !matched)
                r.error = QStringLiteral("No document with that _id any more: deleted meanwhile?");
            return r;
        },
        [this, then](const Result &r) {
            if(then)
                then(r.error.isEmpty());
            if(r.error.isEmpty())
                run();
        });
    return true;
}

void MongoTab::insertDocument(const QString &initial)
{
    if(m_collection.isEmpty())
        return;
    QString text = initial.isEmpty() ? QStringLiteral("{\n  \n}") : initial;
    QString problem;
    for(;;) {
        if(!documentDialog(this, QStringLiteral("Insert Document — %1").arg(m_collection), &text,
                           false, problem))
            return;
        QString error;
        if(insertText(text, {}, &error))
            return;
        problem = error;
    }
}

bool MongoTab::insertText(const QString &text, const std::function<void(bool)> &then,
                          QString *error)
{
    MongoJson::Value doc;
    QString e;
    if(!MongoJson::parseDocument(text, &doc, &e)) {
        if(error)
            *error = e;
        return false;
    }
    /* the _id it gets is known before it goes */
    if(!doc.member(QStringLiteral("_id")))
        doc.members.insert(
            doc.members.begin(),
            {QStringLiteral("_id"),
             MongoJson::Value::object(
                 {{QStringLiteral("$oid"), MongoJson::Value::string(MongoJson::newObjectId())}})});
    const QByteArray canonical = MongoJson::toCanonical(doc);
    const QString db = m_db, coll = m_collection;
    async(
        QStringLiteral("Inserting…"),
        [db, coll, canonical](MongoClient &c) {
            Result r;
            if(c.insert(db, coll, {canonical}, &r.error))
                r.message = QStringLiteral("Inserted one document");
            return r;
        },
        [this, then](const Result &r) {
            if(then)
                then(r.error.isEmpty());
            if(r.error.isEmpty())
                run();
        });
    return true;
}

void MongoTab::deleteSelected()
{
    const QList<int> rows = selectedRows();
    if(!m_editable || rows.isEmpty())
        return;
    if(QMessageBox::question(this, QStringLiteral("Delete Documents"),
                             QStringLiteral("Delete %1 document%2 from %3.%4?")
                                 .arg(rows.size())
                                 .arg(rows.size() == 1 ? "" : "s")
                                 .arg(m_db, m_collection)) != QMessageBox::Yes)
        return;
    QString error;
    if(!deleteRows(rows, {}, &error))
        setStatus(error, true);
}

bool MongoTab::deleteRows(const QList<int> &rows, const std::function<void(bool)> &then,
                          QString *error)
{
    QList<QByteArray> filters;
    for(int row : rows) {
        const MongoJson::Value *id = row >= 0 && row < int(m_docs.size())
                                         ? m_docs[size_t(row)].member(QStringLiteral("_id"))
                                         : nullptr;
        if(!id) {
            if(error)
                *error = QStringLiteral("row %1 has no _id to delete it by").arg(row + 1);
            return false;
        }
        filters << idFilter(*id);
    }
    const QString db = m_db, coll = m_collection;
    async(
        QStringLiteral("Deleting…"),
        [db, coll, filters](MongoClient &c) {
            Result r;
            qint64 total = 0;
            for(const QByteArray &f : filters) {
                qint64 n = 0;
                if(!c.remove(db, coll, f, false, &n, &r.error))
                    break;
                total += n;
            }
            if(r.error.isEmpty())
                r.message =
                    QStringLiteral("Deleted %1 document%2").arg(total).arg(total == 1 ? "" : "s");
            return r;
        },
        [this, then](const Result &r) {
            if(then)
                then(r.error.isEmpty());
            run();
        });
    return true;
}

bool MongoTab::saveDocumentForTest(int row, const QString &text, QString *error)
{
    bool ok = false;
    if(row < 0 || row >= int(m_docs.size()) ||
       !saveDocument(row, text, [&ok](bool done) { ok = done; }, error))
        return false;
    waitForIdle();
    if(!ok && error)
        *error = statusText();
    return ok;
}

bool MongoTab::insertDocumentForTest(const QString &text, QString *error)
{
    bool ok = false;
    if(!insertText(text, [&ok](bool done) { ok = done; }, error))
        return false;
    waitForIdle();
    if(!ok && error)
        *error = statusText();
    return ok;
}

bool MongoTab::deleteRowsForTest(const QList<int> &rows, QString *error)
{
    bool ok = false;
    if(!deleteRows(rows, [&ok](bool done) { ok = done; }, error))
        return false;
    waitForIdle();
    if(!ok && error)
        *error = statusText();
    return ok;
}

/* ---- indexes, export, import ----------------------------------------- */

void MongoTab::manageIndexes()
{
    if(m_collection.isEmpty())
        return;
    const QString db = m_db, coll = m_collection;
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Indexes — %1").arg(coll));
    dlg.resize(620, 320);
    auto *table = new QTableWidget(0, 3, &dlg);
    table->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Keys"), QStringLiteral("Options")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->hide();
    auto *note = new QLabel(&dlg);
    note->setWordWrap(true);

    const std::function<void()> reload = [&, table] {
        QList<QByteArray> indexes;
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_client->indexes(db, coll, &indexes, &error);
        QApplication::restoreOverrideCursor();
        note->setText(error);
        table->setRowCount(int(indexes.size()));
        for(int i = 0; i < indexes.size(); ++i) {
            MongoJson::Value index = parsed(indexes[i]);
            const MongoJson::Value *name = index.member(QStringLiteral("name"));
            const MongoJson::Value *key = index.member(QStringLiteral("key"));
            MongoJson::Value options = MongoJson::Value::object();
            for(const auto &m : index.members)
                if(m.first != QStringLiteral("name") && m.first != QStringLiteral("key") &&
                   m.first != QStringLiteral("v"))
                    options.members.push_back(m);
            table->setItem(i, 0, new QTableWidgetItem(name ? name->text : QString()));
            table->setItem(i, 1,
                           new QTableWidgetItem(key ? MongoJson::toShell(*key, false) : QString()));
            table->setItem(i, 2,
                           new QTableWidgetItem(options.members.empty()
                                                    ? QString()
                                                    : MongoJson::toShell(options, false)));
        }
        table->resizeColumnsToContents();
    };

    auto *create = new QPushButton(Icons::get(QStringLiteral("indexcreate.ico")),
                                   QStringLiteral("&Create…"), &dlg);
    connect(create, &QPushButton::clicked, &dlg, [&, reload] {
        QDialog form(&dlg);
        form.setWindowTitle(QStringLiteral("Create Index"));
        auto *keys = new QLineEdit(QStringLiteral("{field: 1}"), &form);
        keys->setFont(fixedFont());
        keys->setToolTip(
            QStringLiteral("1 ascending, -1 descending, 'text', '2dsphere', 'hashed'"));
        auto *name = new QLineEdit(&form);
        name->setPlaceholderText(QStringLiteral("the server's own: field_1"));
        auto *unique = new QCheckBox(QStringLiteral("&Unique"), &form);
        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &form);
        connect(buttons, &QDialogButtonBox::accepted, &form, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &form, &QDialog::reject);
        auto *fl = new QFormLayout(&form);
        fl->addRow(QStringLiteral("&Keys"), keys);
        fl->addRow(QStringLiteral("&Name"), name);
        fl->addRow(QString(), unique);
        fl->addRow(buttons);
        if(form.exec() != QDialog::Accepted)
            return;
        MongoJson::Value k;
        QString error;
        if(MongoJson::parseDocument(keys->text(), &k, &error)) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            m_client->createIndex(db, coll, MongoJson::toCanonical(k), name->text(),
                                  unique->isChecked(), &error);
            QApplication::restoreOverrideCursor();
        }
        if(!error.isEmpty())
            QMessageBox::warning(&dlg, QStringLiteral("Create Index"), error);
        reload();
    });
    auto *drop = new QPushButton(Icons::get(QStringLiteral("indexdelete.ico")),
                                 QStringLiteral("&Drop"), &dlg);
    connect(drop, &QPushButton::clicked, &dlg, [&, reload] {
        const int row = table->currentRow();
        const QString name =
            row >= 0 && table->item(row, 0) ? table->item(row, 0)->text() : QString();
        if(name.isEmpty() || name == QStringLiteral("_id_"))
            return; /* every collection keeps its _id index */
        if(QMessageBox::question(&dlg, QStringLiteral("Drop Index"),
                                 QStringLiteral("Drop the index %1?").arg(name)) !=
           QMessageBox::Yes)
            return;
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_client->dropIndex(db, coll, name, &error);
        QApplication::restoreOverrideCursor();
        if(!error.isEmpty())
            QMessageBox::warning(&dlg, QStringLiteral("Drop Index"), error);
        reload();
    });
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(close, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(create);
    buttons->addWidget(drop);
    buttons->addStretch(1);
    buttons->addWidget(close);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(table, 1);
    lay->addWidget(note);
    lay->addLayout(buttons);
    reload();
    dlg.exec();
}

void MongoTab::exportCollection()
{
    if(m_collection.isEmpty())
        return;
    MongoJson::Value filter;
    QString error;
    if(!MongoJson::parseDocument(m_mode == Mode::Find ? m_filter->text() : QString(), &filter,
                                 &error)) {
        setStatus(QStringLiteral("Filter: ") + error, true);
        return;
    }
    const QString file = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export %1").arg(m_collection), m_collection + QStringLiteral(".json"),
        QStringLiteral("JSON, one document per line (*.json *.jsonl);;All files (*)"));
    if(file.isEmpty())
        return;
    MongoClient::Find f;
    f.filter = MongoJson::toCanonical(filter);
    const QString db = m_db, coll = m_collection;
    async(
        QStringLiteral("Exporting…"),
        [db, coll, f, file](MongoClient &c) {
            Result r;
            QSaveFile out(file);
            if(!out.open(QIODevice::WriteOnly)) {
                r.error = QStringLiteral("Can't write %1: %2").arg(file, out.errorString());
                return r;
            }
            qint64 n = 0;
            /* canonical Extended JSON: every type comes back as it was, and
             * mongoimport reads it */
            const bool ok = c.find(
                db, coll, f,
                [&](const QByteArray &doc) {
                    ++n;
                    return out.write(doc + '\n') >= 0;
                },
                &r.error);
            if(ok && r.error.isEmpty() && out.commit())
                r.message = QStringLiteral("Exported %1 documents to %2").arg(n).arg(file);
            else if(r.error.isEmpty())
                r.error = QStringLiteral("Can't write %1: %2").arg(file, out.errorString());
            return r;
        },
        [](const Result &) {});
}

void MongoTab::importCollection()
{
    if(m_collection.isEmpty())
        return;
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import into %1").arg(m_collection), QString(),
        QStringLiteral("JSON documents (*.json *.jsonl);;All files (*)"));
    if(file.isEmpty())
        return;
    const QString db = m_db, coll = m_collection;
    async(
        QStringLiteral("Importing…"),
        [db, coll, file](MongoClient &c) {
            Result r;
            QFile in(file);
            if(!in.open(QIODevice::ReadOnly)) {
                r.error = QStringLiteral("Can't read %1: %2").arg(file, in.errorString());
                return r;
            }
            const QString text = QString::fromUtf8(in.readAll());
            QList<QByteArray> batch;
            qint64 n = 0;
            const auto flush = [&] {
                if(batch.isEmpty() || c.insert(db, coll, batch, &r.error)) {
                    n += batch.size();
                    batch.clear();
                    return true;
                }
                return false;
            };
            /* a JSON array of documents, or one document per line */
            if(text.trimmed().startsWith(QLatin1Char('['))) {
                MongoJson::Value all;
                if(!MongoJson::parse(text, &all, &r.error) || !all.isArray()) {
                    if(r.error.isEmpty())
                        r.error = QStringLiteral("not an array of documents");
                    return r;
                }
                for(size_t i = 0; i < all.items.size(); ++i) {
                    if(!all.items[i].isObject()) {
                        r.error = QStringLiteral("item %1 isn't a document").arg(i + 1);
                        return r;
                    }
                    batch << MongoJson::toCanonical(all.items[i]);
                    if(batch.size() >= 500 && !flush())
                        return r;
                }
            } else {
                const QStringList lines = text.split(QLatin1Char('\n'));
                for(int i = 0; i < lines.size(); ++i) {
                    if(lines[i].trimmed().isEmpty())
                        continue;
                    MongoJson::Value doc;
                    QString error;
                    if(!MongoJson::parseDocument(lines[i], &doc, &error)) {
                        r.error = QStringLiteral("line %1: %2").arg(i + 1).arg(error);
                        flush();
                        r.message = QStringLiteral("%1 documents imported before it").arg(n);
                        return r;
                    }
                    batch << MongoJson::toCanonical(doc);
                    if(batch.size() >= 500 && !flush())
                        return r;
                }
            }
            if(flush())
                r.message = QStringLiteral("Imported %1 documents").arg(n);
            return r;
        },
        [this](const Result &) {
            m_skip = 0;
            run();
        });
}
