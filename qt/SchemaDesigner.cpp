/* OpenYog — the Schema Designer tab; see SchemaDesigner.h. */
#include "SchemaDesigner.h"
#include "Icons.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

SchemaDesigner::SchemaDesigner(const QString &database, Loader loader, QWidget *parent)
    : QWidget(parent), m_database(database), m_loader(std::move(loader))
{
    readColors();
    m_scene = new QGraphicsScene(this);
    m_view = new Diagram::View(m_scene, &m_colors, this);
    m_view->setObjectName(QStringLiteral("schemaDesignerView"));
    m_view->zoomChanged = [this] {
        m_zoom->setText(QStringLiteral("%1%").arg(qRound(m_view->zoom() * 100)));
    };
    m_view->contextMenu = [this](const QPoint &pos) { showContextMenu(pos); };
    /* fitted to the window until zoomed by hand: the tab's final size only
     * settles after it is shown (and the result pane steps aside) */
    m_view->resized = [this] {
        if(m_autoFit)
            fitToWindow();
    };
    m_view->userZoomed = [this] { m_autoFit = false; };
    m_view->tableDoubleClicked = [this](Diagram::Table *t) {
        emit alterTableRequested(m_database, t->name());
    };

    auto *bar = new QToolBar(this);
    bar->setIconSize(QSize(16, 16));
    bar->addAction(Icons::get(QStringLiteral("refreshcanvas.ico")), QStringLiteral("Refresh"), this,
                   &SchemaDesigner::reload)
        ->setToolTip(QStringLiteral("Refresh: read the schema again"));
    bar->addAction(QStringLiteral("Auto Arrange"), this, &SchemaDesigner::autoArrange)
        ->setToolTip(QStringLiteral("Lay the tables out again"));
    bar->addSeparator();
    /* shortcuts that only work inside this tab: several can be open */
    const auto zoomAction = [&](const char *icon, const QString &text,
                                QKeySequence::StandardKey key, qreal factor) {
        QAction *a =
            bar->addAction(Icons::get(QString::fromLatin1(icon)), text, this, [this, factor] {
                m_autoFit = false;
                m_view->setZoom(m_view->zoom() * factor);
            });
        a->setShortcut(key);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(a);
    };
    zoomAction("zoomin.ico", QStringLiteral("Zoom In"), QKeySequence::ZoomIn, 1.25);
    zoomAction("zoomout.ico", QStringLiteral("Zoom Out"), QKeySequence::ZoomOut, 1 / 1.25);
    bar->addAction(QStringLiteral("Fit"), this,
                   [this] {
                       m_autoFit = true;
                       fitToWindow();
                   })
        ->setToolTip(QStringLiteral("Fit the whole diagram in the window"));
    bar->addAction(QStringLiteral("100%"), this, [this] {
        m_autoFit = false;
        m_view->setZoom(1);
    });
    m_zoom = new QLabel(this);
    m_zoom->setMinimumWidth(44);
    m_zoom->setAlignment(Qt::AlignCenter);
    bar->addWidget(m_zoom);
    bar->addSeparator();
    bar->addAction(Icons::get(QStringLiteral("Exportasimg.ico")), QStringLiteral("Export As Image"),
                   this, &SchemaDesigner::promptExportImage)
        ->setToolTip(QStringLiteral("Export As Image…"));
    bar->addSeparator();
    m_find = new QLineEdit(this);
    m_find->setPlaceholderText(QStringLiteral("Find table"));
    m_find->setClearButtonEnabled(true);
    m_find->setMaximumWidth(200);
    connect(m_find, &QLineEdit::returnPressed, this, &SchemaDesigner::findNext);
    connect(m_find, &QLineEdit::textEdited, this, [this] {
        m_scene->clearSelection();
        findNext();
    });
    bar->addWidget(m_find);
    auto *spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    m_info = new QLabel(this);
    m_info->setContentsMargins(0, 0, 8, 0);
    bar->addWidget(m_info);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(bar);
    lay->addWidget(m_view, 1);
    m_view->zoomChanged();
}

void SchemaDesigner::readColors()
{
    m_colors = Diagram::colorsFrom(palette());
}

void SchemaDesigner::changeEvent(QEvent *event)
{
    if(event->type() == QEvent::PaletteChange) {
        readColors();
        if(m_scene) {
            m_scene->update();
            m_view->viewport()->update();
        }
    }
    QWidget::changeEvent(event);
}

void SchemaDesigner::reload()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_model = m_loader();
    QApplication::restoreOverrideCursor();
    QSet<QString> names;
    for(const SchemaDiagram::Table &t : std::as_const(m_model.tables))
        names.insert(t.name);
    m_hidden.intersect(names);
    rebuild(true);
    if(m_autoFit)
        fitToWindow();
}

void SchemaDesigner::autoArrange()
{
    m_positions.clear();
    rebuild(false);
    m_autoFit = true;
    fitToWindow();
}

void SchemaDesigner::rebuild(bool keepPositions)
{
    for(auto it = m_items.cbegin(); it != m_items.cend(); ++it)
        m_positions.insert(it.key(), it.value()->pos());
    m_scene->clear();
    m_items.clear();

    SchemaDiagram::Model shown;
    for(const SchemaDiagram::Table &t : std::as_const(m_model.tables))
        if(!m_hidden.contains(t.name))
            shown.tables.append(t);
    QHash<QString, QSizeF> sizes;
    bool allPlaced = true;
    for(const SchemaDiagram::Table &t : std::as_const(shown.tables)) {
        auto *item = new Diagram::Table(t, &m_colors, font());
        m_items.insert(t.name, item);
        sizes.insert(t.name, item->size());
        allPlaced = allPlaced && m_positions.contains(t.name);
    }
    for(const SchemaDiagram::Link &l : std::as_const(m_model.links))
        if(m_items.contains(l.table) && m_items.contains(l.refTable))
            shown.links.append(l);

    /* a table that wasn't on the canvas before re-arranges the lot */
    const QHash<QString, QPointF> pos =
        keepPositions && allPlaced ? m_positions : SchemaDiagram::layout(shown, sizes);
    for(auto it = m_items.cbegin(); it != m_items.cend(); ++it) {
        it.value()->setPos(pos.value(it.key()));
        m_scene->addItem(it.value());
    }
    for(const SchemaDiagram::Link &l : std::as_const(shown.links)) {
        Diagram::Table *from = m_items.value(l.table), *to = m_items.value(l.refTable);
        auto *link = new Diagram::Link(from, l.columns.value(0), to, l.refColumns.value(0),
                                       &m_colors, Diagram::Link::Style::ForeignKey);
        link->setToolTip(QStringLiteral("%1\n%2 (%3) → %4 (%5)")
                             .arg(l.name, l.table, l.columns.join(QStringLiteral(", ")), l.refTable,
                                  l.refColumns.join(QStringLiteral(", "))));
        from->addLink(link);
        if(to != from)
            to->addLink(link);
        m_scene->addItem(link);
    }
    m_linkCount = int(shown.links.size());
    if(m_items.isEmpty()) {
        QGraphicsTextItem *t = m_scene->addText(
            m_model.tables.isEmpty()
                ? QStringLiteral("No tables in %1.").arg(m_database)
                : QStringLiteral("Every table is hidden: right-click ▸ Show All Tables."));
        t->setDefaultTextColor(m_colors.text);
    }
    m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-200, -200, 200, 200));
    updateInfo();
}

void SchemaDesigner::hideTable(const QString &table)
{
    m_hidden.insert(table);
    rebuild(true);
}

void SchemaDesigner::fitToWindow()
{
    const QRectF r = m_scene->itemsBoundingRect().adjusted(-24, -24, 24, 24);
    if(r.isEmpty())
        return;
    m_view->fitInView(r, Qt::KeepAspectRatio);
    m_view->setZoom(std::min<qreal>(m_view->zoom(), 1)); /* never blow a small one up */
    m_view->centerOn(r.center());
}

void SchemaDesigner::findNext()
{
    const QString text = m_find->text().trimmed();
    if(text.isEmpty())
        return;
    /* the next match after the selected one, in table order */
    QStringList matches;
    for(const SchemaDiagram::Table &t : std::as_const(m_model.tables))
        if(m_items.contains(t.name) && t.name.contains(text, Qt::CaseInsensitive))
            matches << t.name;
    if(matches.isEmpty())
        return;
    const QList<QGraphicsItem *> sel = m_scene->selectedItems();
    const auto *current = sel.size() == 1 ? dynamic_cast<Diagram::Table *>(sel.first()) : nullptr;
    const int at = current ? int(matches.indexOf(current->name())) : -1;
    Diagram::Table *next = m_items.value(matches.value((at + 1) % matches.size()));
    m_scene->clearSelection();
    next->setSelected(true);
    m_view->centerOn(next);
}

void SchemaDesigner::updateInfo()
{
    QString text = QStringLiteral("%1 — %2 tables, %3 relationships")
                       .arg(m_database)
                       .arg(m_items.size())
                       .arg(m_linkCount);
    if(!m_hidden.isEmpty())
        text += QStringLiteral(" (%1 hidden)").arg(m_hidden.size());
    m_info->setText(text);
}

void SchemaDesigner::showContextMenu(const QPoint &viewPos)
{
    QMenu menu(this);
    if(Diagram::Table *t = m_view->tableAt(viewPos)) {
        const QString name = t->name();
        if(!t->isSelected()) {
            m_scene->clearSelection();
            t->setSelected(true);
        }
        menu.addAction(Icons::get(QStringLiteral("table.ico")), QStringLiteral("Open Table Data"),
                       this, [this, name] { emit openTableRequested(m_database, name); });
        menu.addAction(Icons::get(QStringLiteral("altertable.ico")), QStringLiteral("Alter Table…"),
                       this, [this, name] { emit alterTableRequested(m_database, name); });
        menu.addAction(Icons::get(QStringLiteral("manrel_16.ico")),
                       QStringLiteral("Relationships/Foreign Keys…"), this,
                       [this, name] { emit foreignKeysRequested(m_database, name); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Hide From Diagram"), this,
                       [this, name] { hideTable(name); });
    } else {
        menu.addAction(QStringLiteral("Show All Tables (%1 hidden)").arg(m_hidden.size()), this,
                       [this] {
                           m_hidden.clear();
                           rebuild(true);
                       })
            ->setEnabled(!m_hidden.isEmpty());
        menu.addAction(QStringLiteral("Auto Arrange"), this, &SchemaDesigner::autoArrange);
        menu.addAction(QStringLiteral("Fit To Window"), this, [this] {
            m_autoFit = true;
            fitToWindow();
        });
        menu.addAction(Icons::get(QStringLiteral("Exportasimg.ico")),
                       QStringLiteral("Export As Image…"), this,
                       &SchemaDesigner::promptExportImage);
        menu.addSeparator();
        menu.addAction(Icons::get(QStringLiteral("refreshcanvas.ico")), QStringLiteral("Refresh"),
                       this, &SchemaDesigner::reload);
    }
    menu.exec(m_view->viewport()->mapToGlobal(viewPos));
}

bool SchemaDesigner::exportImage(const QString &file)
{
    if(m_scene->items().isEmpty())
        return false;
    /* without the selection outline */
    const QList<QGraphicsItem *> selected = m_scene->selectedItems();
    m_scene->clearSelection();
    const QRectF source = m_scene->itemsBoundingRect().adjusted(-24, -24, 24, 24);
    /* twice the screen size for a sharp image, kept under 16000px a side */
    const qreal scale = std::min<qreal>(2, 16000 / std::max(source.width(), source.height()));
    QImage image((source.size() * scale).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.fill(m_colors.canvas);
    QPainter p(&image);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                     QPainter::SmoothPixmapTransform);
    m_scene->render(&p, QRectF(QPointF(0, 0), image.size()), source);
    p.end();
    for(QGraphicsItem *it : selected)
        it->setSelected(true);
    return image.save(file);
}

void SchemaDesigner::promptExportImage()
{
    const QString file = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export As Image"), m_database + QStringLiteral(".png"),
        QStringLiteral("PNG image (*.png);;JPEG image (*.jpg)"));
    if(file.isEmpty())
        return;
    if(!exportImage(file))
        QMessageBox::warning(this, QStringLiteral("Export As Image"),
                             QStringLiteral("Could not write %1").arg(file));
}

QStringList SchemaDesigner::dumpForTest() const
{
    int overlaps = 0;
    const QList<Diagram::Table *> items = m_items.values();
    for(int i = 0; i < items.size(); ++i)
        for(int j = i + 1; j < items.size(); ++j)
            if(QRectF(items[i]->pos(), items[i]->size())
                   .intersects(QRectF(items[j]->pos(), items[j]->size())))
                ++overlaps;
    QStringList out;
    int links = 0;
    for(const SchemaDiagram::Link &l : m_model.links)
        if(m_items.contains(l.table) && m_items.contains(l.refTable)) {
            ++links;
            out << QStringLiteral("schemadesigner link %1: %2 (%3) -> %4 (%5)")
                       .arg(l.name, l.table, l.columns.join(QStringLiteral(", ")), l.refTable,
                            l.refColumns.join(QStringLiteral(", ")));
        }
    out.prepend(QStringLiteral("schemadesigner: database=%1 tables=%2 links=%3 overlaps=%4")
                    .arg(m_database)
                    .arg(m_items.size())
                    .arg(links)
                    .arg(overlaps));
    return out;
}
