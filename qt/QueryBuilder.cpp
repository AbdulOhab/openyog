/* OpenYog — the Query Builder tab; see QueryBuilder.h. */
#include "QueryBuilder.h"
#include "Icons.h"
#include "SqlEditor.h"
#include <Qsci/qsciscintilla.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

using QueryBuild::JoinType;

namespace {

/* hands a dragged table over as its name in plain text, which the canvas
 * takes */
class TableList : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    QStringList mimeTypes() const override
    {
        return {QStringLiteral("text/plain")};
    }
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override
    {
        auto *data = new QMimeData;
        if(!items.isEmpty())
            data->setText(items.first()->text());
        return data;
    }
};

enum GridColumn { GColumn, GAlias, GOutput, GAggregate, GSort, GCriteria, GOr, GCount };

const QStringList kAggregates = {QString(),
                                 QStringLiteral("COUNT"),
                                 QStringLiteral("SUM"),
                                 QStringLiteral("AVG"),
                                 QStringLiteral("MIN"),
                                 QStringLiteral("MAX")};

} // namespace

QueryBuilder::QueryBuilder(const QString &database, Backend backend, QWidget *parent)
    : QWidget(parent), m_database(database), m_backend(std::move(backend))
{
    m_colors = Diagram::colorsFrom(palette());
    m_scene = new QGraphicsScene(this);
    m_view = new Diagram::View(m_scene, &m_colors, this);
    m_view->setObjectName(QStringLiteral("queryBuilderView"));
    /* tables go in from the top left, the way they are added */
    m_view->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_view->contextMenu = [this](const QPoint &pos) { showCanvasMenu(pos); };
    m_view->textDropped = [this](const QString &text, const QPointF &pos) {
        addTableAt(text.trimmed(), pos, true);
    };

    /* the tables, to double-click or drag onto the canvas */
    auto *left = new QWidget(this);
    auto *leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(2);
    m_filter = new QLineEdit(left);
    m_filter->setPlaceholderText(QStringLiteral("Filter tables"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &QueryBuilder::filterTables);
    m_tableList = new TableList(left);
    m_tableList->setDragEnabled(true);
    m_tableList->setDragDropMode(QAbstractItemView::DragOnly);
    m_tableList->setToolTip(QStringLiteral("Double-click a table, or drag it onto the canvas"));
    connect(m_tableList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { addTable(item->text()); });
    leftLay->addWidget(m_filter);
    leftLay->addWidget(m_tableList, 1);

    /* the chosen columns */
    m_grid = new QTableWidget(0, GCount, this);
    m_grid->setHorizontalHeaderLabels({QStringLiteral("Column"), QStringLiteral("Alias"),
                                       QStringLiteral("Output"), QStringLiteral("Aggregate"),
                                       QStringLiteral("Sort"), QStringLiteral("Criteria"),
                                       QStringLiteral("Or")});
    m_grid->verticalHeader()->hide();
    m_grid->horizontalHeader()->setStretchLastSection(true);
    m_grid->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setColumnWidth(GAlias, 100);
    m_grid->setColumnWidth(GOutput, 56);
    m_grid->setColumnWidth(GAggregate, 96);
    m_grid->setColumnWidth(GSort, 108);
    m_grid->setColumnWidth(GCriteria, 150);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->setToolTip(QStringLiteral("Criteria: SQL as typed, e.g.  > 100   LIKE 'A%'   IS NULL"
                                      "   — a plain value compares with =\n"
                                      "Or: a second set of conditions; a row is kept when it "
                                      "meets all of Criteria or all of Or"));
    connect(m_grid, &QWidget::customContextMenuRequested, this, &QueryBuilder::showGridMenu);
    connect(m_grid, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if(m_fillingGrid || item->row() >= m_columns.size())
            return;
        QueryBuild::Column &c = m_columns[item->row()];
        if(item->column() == GAlias)
            c.alias = item->text().trimmed();
        else if(item->column() == GOutput)
            c.output = item->checkState() == Qt::Checked;
        else if(item->column() == GCriteria)
            c.criteria = item->text();
        else if(item->column() == GOr)
            c.orCriteria = item->text();
        updateSql();
    });

    m_sql = new SqlEditor(this);
    m_sql->setReadOnly(true);
    m_sql->setWrapMode(QsciScintilla::WrapWord);

    auto *bar = new QToolBar(this);
    bar->setIconSize(QSize(16, 16));
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bar->addAction(Icons::get(QStringLiteral("open.ico")), QStringLiteral("Open…"), this,
                   &QueryBuilder::promptOpen)
        ->setToolTip(QStringLiteral("Open a saved query (.oyqb)"));
    bar->addAction(Icons::get(QStringLiteral("save.ico")), QStringLiteral("Save…"), this,
                   &QueryBuilder::promptSave)
        ->setToolTip(QStringLiteral("Save the query and its tables' places (.oyqb)"));
    bar->addSeparator();
    bar->addAction(Icons::get(QStringLiteral("refresh_16.ico")), QStringLiteral("Refresh"), this,
                   &QueryBuilder::reloadTables)
        ->setToolTip(QStringLiteral("Read the list of tables again"));
    bar->addSeparator();
    m_distinct = new QCheckBox(QStringLiteral("Distinct"), this);
    connect(m_distinct, &QCheckBox::toggled, this, &QueryBuilder::updateSql);
    bar->addWidget(m_distinct);
    bar->addWidget(new QLabel(QStringLiteral("  Limit "), this));
    m_limit = new QSpinBox(this);
    m_limit->setRange(0, 1000000000);
    m_limit->setSpecialValueText(QStringLiteral("none"));
    connect(m_limit, &QSpinBox::valueChanged, this, &QueryBuilder::updateSql);
    bar->addWidget(m_limit);
    bar->addSeparator();
    bar->addAction(Icons::get(QStringLiteral("query_16.ico")),
                   QStringLiteral("Copy to Query Editor"), this,
                   [this] { emit copyToEditorRequested(sql()); });
    bar->addAction(Icons::get(QStringLiteral("execute_16.ico")), QStringLiteral("Run"), this,
                   [this] { emit runRequested(sql()); });
    bar->addSeparator();
    bar->addAction(QStringLiteral("Clear"), this, &QueryBuilder::clearAll);

    auto *top = new QSplitter(Qt::Horizontal, this);
    top->addWidget(left);
    top->addWidget(m_view);
    top->setStretchFactor(1, 1);
    top->setSizes({180, 800});
    auto *bottom = new QSplitter(Qt::Horizontal, this);
    bottom->addWidget(m_grid);
    bottom->addWidget(m_sql);
    bottom->setSizes({640, 360});
    auto *split = new QSplitter(Qt::Vertical, this);
    split->addWidget(top);
    split->addWidget(bottom);
    split->setSizes({360, 220});

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(bar);
    lay->addWidget(split, 1);

    reloadTables();
    rebuildCanvas();
    updateSql();
}

void QueryBuilder::changeEvent(QEvent *event)
{
    if(event->type() == QEvent::PaletteChange && m_scene) {
        m_colors = Diagram::colorsFrom(palette());
        m_scene->update();
        m_view->viewport()->update();
    }
    QWidget::changeEvent(event);
}

/* where the tables are now, before the canvas is rebuilt */
void QueryBuilder::savePositions()
{
    for(int i = 0; i < m_items.size(); ++i)
        m_sources[i].pos = m_items[i]->pos();
}

void QueryBuilder::clearAll()
{
    m_sources.clear();
    m_joins.clear();
    m_columns.clear();
    rebuildCanvas();
    rebuildGrid();
    updateSql();
}

void QueryBuilder::reloadTables()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QStringList tables = m_backend.tables();
    m_foreignKeys = m_backend.foreignKeys();
    QApplication::restoreOverrideCursor();
    m_tableList->clear();
    for(const QString &t : tables)
        m_tableList->addItem(new QListWidgetItem(Icons::get(QStringLiteral("table.ico")), t));
    filterTables(m_filter->text());
}

bool QueryBuilder::saveFile(const QString &file, QString *error)
{
    savePositions();
    QJsonObject json = QueryBuild::toJson(query());
    QJsonArray positions;
    for(const Instance &in : std::as_const(m_sources))
        positions.append(QJsonArray{in.pos.x(), in.pos.y()});
    json.insert(QStringLiteral("positions"), positions);
    QSaveFile f(file);
    if(!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(json).toJson()) < 0 || !f.commit()) {
        if(error)
            *error = f.errorString();
        return false;
    }
    m_file = file;
    return true;
}

bool QueryBuilder::openFile(const QString &file, QString *error, QStringList *leftOut)
{
    QFile f(file);
    if(!f.open(QIODevice::ReadOnly)) {
        if(error)
            *error = f.errorString();
        return false;
    }
    const QJsonObject json = QJsonDocument::fromJson(f.readAll()).object();
    QueryBuild::Query saved;
    if(!QueryBuild::fromJson(json, &saved, error))
        return false;

    /* the saved tables as they are now; the ones gone are left out, and what
     * refers to them */
    QList<Instance> sources;
    QList<int> index; /* saved source → new one, -1: left out */
    const QJsonArray positions = json.value(QStringLiteral("positions")).toArray();
    QStringList gone;
    for(int i = 0; i < saved.sources.size(); ++i) {
        Instance in;
        in.def = m_backend.table(saved.sources[i].table);
        if(in.def.columns.isEmpty()) {
            gone << QStringLiteral("table %1").arg(saved.sources[i].table);
            index << -1;
            continue;
        }
        in.alias = saved.sources[i].alias;
        const QJsonArray xy = positions.at(i).toArray();
        in.pos = QPointF(xy.at(0).toDouble(20), xy.at(1).toDouble(20));
        index << int(sources.size());
        sources << in;
    }
    const auto has = [&](int s, const QString &column) {
        for(const SchemaDiagram::Column &c : std::as_const(sources[s].def.columns))
            if(c.name == column)
                return true;
        return false;
    };
    const auto ref = [&](int s) {
        return sources[s].alias.isEmpty() ? sources[s].def.name : sources[s].alias;
    };
    QList<QueryBuild::Join> joins;
    for(QueryBuild::Join j : std::as_const(saved.joins)) {
        j.left = index.value(j.left, -1);
        j.right = index.value(j.right, -1);
        if(j.left < 0 || j.right < 0)
            continue;
        QList<QPair<QString, QString>> on;
        for(const auto &pair : std::as_const(j.on)) {
            if(has(j.left, pair.first) && has(j.right, pair.second))
                on << pair;
            else
                gone << QStringLiteral("join %1.%2 = %3.%4")
                            .arg(ref(j.left), pair.first, ref(j.right), pair.second);
        }
        j.on = on;
        if(!j.on.isEmpty())
            joins << j;
    }
    QList<QueryBuild::Column> columns;
    for(QueryBuild::Column c : std::as_const(saved.columns)) {
        c.source = index.value(c.source, -1);
        if(c.source < 0)
            continue;
        if(!has(c.source, c.column)) {
            gone << QStringLiteral("column %1.%2").arg(ref(c.source), c.column);
            continue;
        }
        columns << c;
    }

    m_sources = sources;
    m_joins = joins;
    m_columns = columns;
    m_distinct->setChecked(saved.distinct);
    m_limit->setValue(saved.limit);
    m_file = file;
    if(leftOut)
        *leftOut = gone;
    rebuildCanvas();
    rebuildGrid();
    updateSql();
    return true;
}

static QString builderFilter()
{
    return QStringLiteral("Query Builder files (*.oyqb);;All files (*)");
}

void QueryBuilder::promptSave()
{
    if(m_sources.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Save Query"),
                                 QStringLiteral("Add a table first: there is nothing to save."));
        return;
    }
    const QString suggested =
        m_file.isEmpty() ? QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                               .filePath(m_database + QStringLiteral(".oyqb"))
                         : m_file;
    QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Save Query"), suggested,
                                                builderFilter());
    if(file.isEmpty())
        return;
    if(QFileInfo(file).suffix().isEmpty())
        file += QStringLiteral(".oyqb");
    QString error;
    if(!saveFile(file, &error))
        QMessageBox::warning(this, QStringLiteral("Save Query"),
                             QStringLiteral("Couldn't save %1:\n%2").arg(file, error));
}

void QueryBuilder::promptOpen()
{
    if(!m_sources.isEmpty() &&
       QMessageBox::question(this, QStringLiteral("Open Query"),
                             QStringLiteral("Replace the query being built?")) != QMessageBox::Yes)
        return;
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Query"),
        m_file.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                         : QFileInfo(m_file).path(),
        builderFilter());
    if(file.isEmpty())
        return;
    QString error;
    QStringList leftOut;
    if(!openFile(file, &error, &leftOut))
        QMessageBox::warning(this, QStringLiteral("Open Query"),
                             QStringLiteral("Couldn't open %1:\n%2").arg(file, error));
    else if(!leftOut.isEmpty())
        QMessageBox::information(
            this, QStringLiteral("Open Query"),
            QStringLiteral("%1 isn't there any more in %2, so it was left out:\n\n%3")
                .arg(leftOut.size() == 1 ? QStringLiteral("This") : QStringLiteral("These"),
                     m_database, leftOut.join(QLatin1Char('\n'))));
}

void QueryBuilder::filterTables(const QString &text)
{
    for(int i = 0; i < m_tableList->count(); ++i)
        m_tableList->item(i)->setHidden(
            !m_tableList->item(i)->text().contains(text.trimmed(), Qt::CaseInsensitive));
}

QString QueryBuilder::sourceTitle(int index) const
{
    const Instance &in = m_sources[index];
    return in.alias.isEmpty() ? in.def.name : in.def.name + QStringLiteral(" AS ") + in.alias;
}

QueryBuild::Query QueryBuilder::query() const
{
    QueryBuild::Query q;
    q.database = m_database;
    for(const Instance &in : m_sources)
        q.sources.append({in.def.name, in.alias});
    q.joins = m_joins;
    q.columns = m_columns;
    q.distinct = m_distinct->isChecked();
    q.limit = m_limit->value();
    return q;
}

QString QueryBuilder::sql() const
{
    return QueryBuild::toSql(query(), m_backend.quote);
}

void QueryBuilder::updateSql()
{
    m_sql->setReadOnly(false);
    m_sql->setPlainText(m_sources.isEmpty()
                            ? QStringLiteral("-- add a table: double-click it on the left, or "
                                             "drag it onto the canvas")
                            : sql());
    m_sql->setReadOnly(true);
}

void QueryBuilder::addTable(const QString &table, const QStringList &columns)
{
    const int before = int(m_sources.size());
    addTableAt(table, {}, false);
    if(m_sources.size() == before)
        return;
    for(const QString &c : columns)
        toggleColumn(before, c, true);
    rebuildCanvas();
}

void QueryBuilder::addTableAt(const QString &table, const QPointF &pos, bool placed)
{
    if(table.isEmpty())
        return;
    Instance in;
    in.def = m_backend.table(table);
    if(in.def.columns.isEmpty())
        return; /* no such table (anything else dropped on the canvas) */

    /* a second copy of a table needs a name of its own */
    QSet<QString> refs;
    int copies = 0;
    for(const Instance &s : std::as_const(m_sources)) {
        refs.insert(s.alias.isEmpty() ? s.def.name : s.alias);
        if(s.def.name == table)
            ++copies;
    }
    if(copies > 0) {
        int n = copies + 1;
        while(refs.contains(table + QString::number(n)))
            ++n;
        in.alias = table + QString::number(n);
    }

    savePositions();
    in.pos = placed ? pos : nextFreeSpot(in);
    m_sources.append(in);
    autoJoin(int(m_sources.size()) - 1);
    rebuildCanvas();
    rebuildGrid();
    updateSql();
    /* once the canvas has its size: a tab opened just now hasn't */
    const int added = int(m_items.size()) - 1;
    QTimer::singleShot(0, this, [this, added] {
        if(added < m_items.size())
            m_view->ensureVisible(m_items[added], 20, 20);
    });
}

/* right of the last table added, or under everything once that would run
 * past the canvas's visible width */
QPointF QueryBuilder::nextFreeSpot(const Instance &in) const
{
    if(m_items.isEmpty())
        return QPointF(20, 20);
    const qreal width = Diagram::Table(in.def, &m_colors, font(), in.def.name, true).size().width();
    const qreal visible = m_view->mapToScene(m_view->viewport()->rect()).boundingRect().right();
    const Diagram::Table *last = m_items.last();
    const qreal x = last->pos().x() + last->size().width() + 60;
    if(x + width <= std::max<qreal>(visible, 600))
        return QPointF(x, last->pos().y());
    qreal bottom = 0;
    for(const Diagram::Table *item : std::as_const(m_items))
        bottom = std::max(bottom, item->pos().y() + item->size().height());
    return QPointF(20, bottom + 40);
}

/* the new table joined along its foreign keys to the tables already there,
 * either way round, but once at most into each group of tables already
 * joined together: a second join into the same group would close a loop and
 * quietly filter the rows. A copy of a table already there takes its
 * self-referencing key first, the usual reason for adding a copy. */
void QueryBuilder::autoJoin(int added)
{
    QList<int> group(added);
    for(int s = 0; s < added; ++s)
        group[s] = s;
    const std::function<int(int)> find = [&](int s) {
        return group[s] == s ? s : group[s] = find(group[s]);
    };
    for(const QueryBuild::Join &j : std::as_const(m_joins))
        if(j.left < added && j.right < added)
            group[find(j.left)] = find(j.right);

    QList<SchemaDiagram::Link> keys = m_foreignKeys;
    std::stable_partition(keys.begin(), keys.end(),
                          [](const SchemaDiagram::Link &k) { return k.table == k.refTable; });
    const QString name = m_sources[added].def.name;
    QSet<int> joinedGroups;
    for(const SchemaDiagram::Link &fk : std::as_const(keys)) {
        for(int s = 0; s < added; ++s) {
            if(joinedGroups.contains(find(s)))
                continue;
            const QString other = m_sources[s].def.name;
            int child = -1, parent = -1;
            if(fk.table == other && fk.refTable == name) {
                child = s;
                parent = added;
            } else if(fk.table == name && fk.refTable == other) {
                child = added;
                parent = s;
            } else {
                continue;
            }
            QueryBuild::Join j;
            j.left = child;
            j.right = parent;
            for(int i = 0; i < fk.columns.size(); ++i)
                j.on.append({fk.columns[i], fk.refColumns.value(i)});
            m_joins.append(j);
            joinedGroups.insert(find(s));
            break; /* this key: the first table it fits */
        }
    }
}

void QueryBuilder::addJoin(int left, const QString &leftColumn, int right,
                           const QString &rightColumn)
{
    savePositions();
    bool added = false;
    for(QueryBuild::Join &j : m_joins) {
        if(j.left == left && j.right == right) {
            j.on.append({leftColumn, rightColumn});
            added = true;
        } else if(j.left == right && j.right == left) {
            j.on.append({rightColumn, leftColumn});
            added = true;
        }
        if(added)
            break;
    }
    if(!added) {
        QueryBuild::Join j;
        j.left = left;
        j.right = right;
        j.on.append({leftColumn, rightColumn});
        m_joins.append(j);
    }
    rebuildCanvas();
    updateSql();
}

void QueryBuilder::removeSource(int index)
{
    savePositions();
    const auto shift = [index](int &s) {
        if(s > index)
            --s;
    };
    for(int i = int(m_joins.size()) - 1; i >= 0; --i) {
        if(m_joins[i].left == index || m_joins[i].right == index) {
            m_joins.removeAt(i);
            continue;
        }
        shift(m_joins[i].left);
        shift(m_joins[i].right);
    }
    for(int i = int(m_columns.size()) - 1; i >= 0; --i) {
        if(m_columns[i].source == index) {
            m_columns.removeAt(i);
            continue;
        }
        shift(m_columns[i].source);
    }
    m_sources.removeAt(index);
    rebuildCanvas();
    rebuildGrid();
    updateSql();
}

void QueryBuilder::toggleColumn(int source, const QString &column, bool on)
{
    if(on) {
        QueryBuild::Column c;
        c.source = source;
        c.column = column;
        m_columns.append(c);
    } else {
        m_columns.erase(std::remove_if(m_columns.begin(), m_columns.end(),
                                       [&](const QueryBuild::Column &c) {
                                           return c.source == source && c.column == column;
                                       }),
                        m_columns.end());
    }
    rebuildGrid();
    updateSql();
}

void QueryBuilder::rebuildCanvas()
{
    m_scene->clear();
    m_items.clear();
    m_itemSource.clear();
    for(int i = 0; i < m_sources.size(); ++i) {
        auto *item = new Diagram::Table(m_sources[i].def, &m_colors, font(), sourceTitle(i), true);
        item->setPos(m_sources[i].pos);
        for(const QueryBuild::Column &c : std::as_const(m_columns))
            if(c.source == i)
                item->setChecked(c.column, true);
        item->toggled = [this, item](const QString &column, bool on) {
            toggleColumn(m_itemSource.value(item), column, on);
        };
        /* later: the drop rebuilds the canvas, this box included, and it is
         * still inside its own mouse release */
        item->joinDropped = [this, item](const QString &column, Diagram::Table *target,
                                         const QString &targetColumn) {
            const int from = m_itemSource.value(item), to = m_itemSource.value(target);
            QMetaObject::invokeMethod(
                this,
                [this, from, column, to, targetColumn] { addJoin(from, column, to, targetColumn); },
                Qt::QueuedConnection);
        };
        m_scene->addItem(item);
        m_items.append(item);
        m_itemSource.insert(item, i);
    }
    for(int j = 0; j < m_joins.size(); ++j) {
        const QueryBuild::Join &join = m_joins[j];
        Diagram::Table *from = m_items.value(join.left), *to = m_items.value(join.right);
        if(!from || !to)
            continue;
        const QString label = join.type == JoinType::Left    ? QStringLiteral("LEFT")
                              : join.type == JoinType::Right ? QStringLiteral("RIGHT")
                                                             : QString();
        for(const auto &pair : join.on) {
            auto *link = new Diagram::Link(from, pair.first, to, pair.second, &m_colors,
                                           Diagram::Link::Style::Join);
            link->setLabel(label);
            link->setData(0, j);
            link->setToolTip(
                QStringLiteral("%1.%2 = %3.%4\nRight-click: join type, remove")
                    .arg(m_sources[join.left].alias.isEmpty() ? from->name()
                                                              : m_sources[join.left].alias,
                         pair.first,
                         m_sources[join.right].alias.isEmpty() ? to->name()
                                                               : m_sources[join.right].alias,
                         pair.second));
            from->addLink(link);
            to->addLink(link);
            m_scene->addItem(link);
        }
    }
    if(m_items.isEmpty()) {
        QGraphicsTextItem *hint = m_scene->addText(
            QStringLiteral("Double-click a table on the left, or drag it here.\n"
                           "Tick the columns you want; drag a column onto another table's "
                           "column to join them."));
        hint->setDefaultTextColor(m_colors.typeText);
    }
    m_scene->setSceneRect(
        m_scene->itemsBoundingRect().united(QRectF(0, 0, 1, 1)).adjusted(-20, -20, 400, 300));
}

void QueryBuilder::rebuildGrid()
{
    m_fillingGrid = true;
    m_grid->setRowCount(int(m_columns.size()));
    for(int r = 0; r < m_columns.size(); ++r) {
        const QueryBuild::Column &c = m_columns[r];
        const Instance &in = m_sources[c.source];
        auto *name = new QTableWidgetItem((in.alias.isEmpty() ? in.def.name : in.alias) +
                                          QLatin1Char('.') + c.column);
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        m_grid->setItem(r, GColumn, name);
        m_grid->setItem(r, GAlias, new QTableWidgetItem(c.alias));
        auto *output = new QTableWidgetItem;
        output->setFlags((output->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        output->setCheckState(c.output ? Qt::Checked : Qt::Unchecked);
        m_grid->setItem(r, GOutput, output);
        m_grid->setItem(r, GCriteria, new QTableWidgetItem(c.criteria));
        m_grid->setItem(r, GOr, new QTableWidgetItem(c.orCriteria));

        auto *aggregate = new QComboBox(m_grid);
        for(const QString &a : kAggregates)
            aggregate->addItem(a.isEmpty() ? QStringLiteral("—") : a, a);
        aggregate->setCurrentIndex(std::max(0, int(kAggregates.indexOf(c.aggregate.toUpper()))));
        connect(aggregate, &QComboBox::currentIndexChanged, this, [this, r, aggregate] {
            m_columns[r].aggregate = aggregate->currentData().toString();
            updateSql();
        });
        m_grid->setCellWidget(r, GAggregate, aggregate);

        auto *sort = new QComboBox(m_grid);
        sort->addItems(
            {QStringLiteral("—"), QStringLiteral("Ascending"), QStringLiteral("Descending")});
        sort->setCurrentIndex(int(c.sort));
        connect(sort, &QComboBox::currentIndexChanged, this, [this, r](int i) {
            m_columns[r].sort = QueryBuild::Sort(i);
            updateSql();
        });
        m_grid->setCellWidget(r, GSort, sort);
    }
    m_grid->resizeColumnToContents(GColumn);
    m_fillingGrid = false;
}

void QueryBuilder::showGridMenu(const QPoint &pos)
{
    const int row = m_grid->rowAt(pos.y());
    if(row < 0 || row >= m_columns.size())
        return;
    QMenu menu(this);
    QAction *up = menu.addAction(QStringLiteral("Move Up"));
    up->setEnabled(row > 0);
    QAction *down = menu.addAction(QStringLiteral("Move Down"));
    down->setEnabled(row + 1 < m_columns.size());
    menu.addSeparator();
    QAction *remove = menu.addAction(QStringLiteral("Remove"));
    QAction *chosen = menu.exec(m_grid->viewport()->mapToGlobal(pos));
    if(!chosen)
        return;
    if(chosen == remove) {
        const QueryBuild::Column c = m_columns.takeAt(row);
        bool still = false;
        for(const QueryBuild::Column &o : std::as_const(m_columns))
            still = still || (o.source == c.source && o.column == c.column);
        if(Diagram::Table *item = m_items.value(c.source))
            item->setChecked(c.column, still);
    } else {
        m_columns.swapItemsAt(row, chosen == up ? row - 1 : row + 1);
    }
    rebuildGrid();
    updateSql();
}

void QueryBuilder::showCanvasMenu(const QPoint &viewPos)
{
    QMenu menu(this);
    if(Diagram::Link *link = m_view->linkAt(viewPos)) {
        const int j = link->data(0).toInt();
        const QString left = m_sources[m_joins[j].left].alias.isEmpty()
                                 ? m_sources[m_joins[j].left].def.name
                                 : m_sources[m_joins[j].left].alias;
        const QString right = m_sources[m_joins[j].right].alias.isEmpty()
                                  ? m_sources[m_joins[j].right].def.name
                                  : m_sources[m_joins[j].right].alias;
        const auto setType = [this, j](JoinType t) {
            savePositions();
            m_joins[j].type = t;
            rebuildCanvas();
            updateSql();
        };
        const auto typeAction = [&](const QString &text, JoinType t) {
            QAction *a = menu.addAction(text, this, [setType, t] { setType(t); });
            a->setCheckable(true);
            a->setChecked(m_joins[j].type == t);
        };
        typeAction(QStringLiteral("Inner Join: matching rows only"), JoinType::Inner);
        typeAction(QStringLiteral("Left Join: every row of %1").arg(left), JoinType::Left);
        typeAction(QStringLiteral("Right Join: every row of %1").arg(right), JoinType::Right);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remove Join"), this, [this, j] {
            savePositions();
            m_joins.removeAt(j);
            rebuildCanvas();
            updateSql();
        });
    } else if(Diagram::Table *table = m_view->tableAt(viewPos)) {
        const int s = m_itemSource.value(table);
        menu.addAction(QStringLiteral("Select All Columns"), this, [this, s] {
            for(const SchemaDiagram::Column &c : std::as_const(m_sources[s].def.columns)) {
                bool present = false;
                for(const QueryBuild::Column &o : std::as_const(m_columns))
                    present = present || (o.source == s && o.column == c.name);
                if(!present) {
                    QueryBuild::Column add;
                    add.source = s;
                    add.column = c.name;
                    m_columns.append(add);
                }
                m_items[s]->setChecked(c.name, true);
            }
            rebuildGrid();
            updateSql();
        });
        menu.addAction(QStringLiteral("Clear Columns"), this, [this, s] {
            for(const SchemaDiagram::Column &c : std::as_const(m_sources[s].def.columns))
                m_items[s]->setChecked(c.name, false);
            m_columns.erase(
                std::remove_if(m_columns.begin(), m_columns.end(),
                               [s](const QueryBuild::Column &c) { return c.source == s; }),
                m_columns.end());
            rebuildGrid();
            updateSql();
        });
        menu.addAction(QStringLiteral("Alias…"), this, [this, s] {
            bool ok = false;
            const QString alias =
                QInputDialog::getText(
                    this, QStringLiteral("Alias"),
                    QStringLiteral("Alias for %1 (empty: none):").arg(m_sources[s].def.name),
                    QLineEdit::Normal, m_sources[s].alias, &ok)
                    .trimmed();
            if(!ok)
                return;
            const QString ref = alias.isEmpty() ? m_sources[s].def.name : alias;
            for(int i = 0; i < m_sources.size(); ++i)
                if(i != s && (m_sources[i].alias.isEmpty() ? m_sources[i].def.name
                                                           : m_sources[i].alias) == ref) {
                    QMessageBox::information(
                        this, QStringLiteral("Alias"),
                        QStringLiteral("%1 already names another table here.").arg(ref));
                    return;
                }
            savePositions();
            m_sources[s].alias = alias == m_sources[s].def.name ? QString() : alias;
            rebuildCanvas();
            rebuildGrid();
            updateSql();
        });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remove Table"), this, [this, s] { removeSource(s); });
    } else {
        menu.addAction(QStringLiteral("Clear"), this, &QueryBuilder::clearAll)
            ->setEnabled(!m_sources.isEmpty());
    }
    menu.exec(m_view->viewport()->mapToGlobal(viewPos));
}
