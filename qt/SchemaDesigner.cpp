/* OpenYog — the Schema Designer tab; see SchemaDesigner.h. */
#include "SchemaDesigner.h"
#include "Icons.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QFontMetricsF>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPathStroker>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

using Colors = SchemaDesigner::Colors;

class DiagramLink;

/* one table: a title bar, then a row per column (key icon, name, type) */
class DiagramTable : public QGraphicsItem
{
public:
    DiagramTable(const SchemaDiagram::Table &table, const Colors *colors, const QFont &font)
        : m_table(table), m_colors(colors), m_font(font), m_bold(font)
    {
        m_bold.setBold(true);
        const QFontMetricsF fm(m_font), bm(m_bold);
        m_rowHeight = std::ceil(fm.height()) + 4;
        m_headerHeight = std::ceil(bm.height()) + 8;
        m_title = bm.elidedText(table.name, Qt::ElideRight, 320);
        qreal nameWidth = 0, typeWidth = 0;
        for(const SchemaDiagram::Column &c : table.columns) {
            const QFontMetricsF &m = c.primaryKey ? bm : fm;
            m_names << m.elidedText(c.name, Qt::ElideRight, 220);
            m_types << fm.elidedText(c.type, Qt::ElideRight, 150);
            nameWidth = std::max(nameWidth, m.horizontalAdvance(m_names.last()));
            typeWidth = std::max(typeWidth, fm.horizontalAdvance(m_types.last()));
        }
        const qreal width = std::max({kText + nameWidth + 18 + typeWidth + 8,
                                      bm.horizontalAdvance(m_title) + 16, qreal(120)});
        m_size =
            QSizeF(std::ceil(width), m_headerHeight + 3 + table.columns.size() * m_rowHeight + 4);

        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setToolTip(QStringLiteral("%1 — %2 columns").arg(table.name).arg(table.columns.size()));
    }

    QRectF boundingRect() const override
    {
        return QRectF(QPointF(-2, -2), m_size + QSizeF(4, 4));
    }
    QSizeF size() const
    {
        return m_size;
    }
    const QString &name() const
    {
        return m_table.name;
    }
    /* a column's row, as a y in the item; the title bar for an unknown one */
    qreal rowCenter(const QString &column) const
    {
        for(int i = 0; i < m_table.columns.size(); ++i)
            if(m_table.columns[i].name == column)
                return m_headerHeight + 3 + (i + 0.5) * m_rowHeight;
        return m_headerHeight / 2;
    }
    void addLink(DiagramLink *link)
    {
        m_links << link;
    }

    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;

private:
    static constexpr qreal kText = 24; /* where column names start, after the icon */

    SchemaDiagram::Table m_table;
    const Colors *m_colors;
    QFont m_font, m_bold;
    QString m_title;
    QStringList m_names, m_types; /* elided to fit */
    qreal m_rowHeight = 0, m_headerHeight = 0;
    QSizeF m_size;
    QList<DiagramLink *> m_links;
};

/* one foreign key: a curve from the referencing column's row to the
 * referenced one's, with a crow's foot at the referencing ("many") end and a
 * bar at the referenced ("one") end */
class DiagramLink : public QGraphicsPathItem
{
public:
    DiagramLink(const SchemaDiagram::Link &link, DiagramTable *from, DiagramTable *to,
                const Colors *colors)
        : m_link(link), m_from(from), m_to(to), m_colors(colors)
    {
        setZValue(-1); /* behind the tables */
        setToolTip(QStringLiteral("%1\n%2 (%3) → %4 (%5)")
                       .arg(link.name, link.table, link.columns.join(QStringLiteral(", ")),
                            link.refTable, link.refColumns.join(QStringLiteral(", "))));
        updatePath();
    }

    void updatePath()
    {
        const QRectF a(m_from->pos(), m_from->size()), b(m_to->pos(), m_to->size());
        const qreal ya = a.top() + m_from->rowCenter(m_link.columns.value(0));
        const qreal yb = b.top() + m_to->rowCenter(m_link.refColumns.value(0));
        QPainterPath path;
        /* side by side: leave and enter on the facing sides; one above the
         * other (or a table referencing itself): loop out to the right */
        const bool stacked = a.left() < b.right() + 30 && b.left() < a.right() + 30;
        if(stacked) {
            m_start = QPointF(a.right(), ya);
            m_end = QPointF(b.right(), yb);
            m_startDir = m_endDir = 1;
            const qreal cx = std::max(a.right(), b.right()) + 40 + std::abs(yb - ya) / 8;
            path.moveTo(m_start);
            path.cubicTo(QPointF(cx, ya), QPointF(cx, yb), m_end);
        } else {
            const bool rightward = a.center().x() < b.center().x();
            m_startDir = rightward ? 1 : -1;
            m_endDir = -m_startDir;
            m_start = QPointF(rightward ? a.right() : a.left(), ya);
            m_end = QPointF(rightward ? b.left() : b.right(), yb);
            const qreal bend = std::max<qreal>(40, std::abs(m_end.x() - m_start.x()) / 2);
            path.moveTo(m_start);
            path.cubicTo(m_start + QPointF(m_startDir * bend, 0),
                         m_end + QPointF(m_endDir * bend, 0), m_end);
        }
        setPath(path);
    }

    QRectF boundingRect() const override
    {
        return path().boundingRect().adjusted(-14, -14, 14, 14);
    }
    QPainterPath shape() const override
    {
        QPainterPathStroker s;
        s.setWidth(8);
        return s.createStroke(path());
    }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        const bool hot = m_from->isSelected() || m_to->isSelected();
        QPen pen(hot ? m_colors->highlight : m_colors->line, hot ? 2 : 1.3);
        pen.setCapStyle(Qt::RoundCap);
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        p->drawPath(path());
        const QPointF fork = m_start + QPointF(m_startDir * 12, 0);
        p->drawLine(fork, m_start + QPointF(0, -6));
        p->drawLine(fork, m_start + QPointF(0, 6));
        const QPointF bar = m_end + QPointF(m_endDir * 8, 0);
        p->drawLine(bar + QPointF(0, -6), bar + QPointF(0, 6));
    }

private:
    SchemaDiagram::Link m_link;
    DiagramTable *m_from, *m_to;
    const Colors *m_colors;
    QPointF m_start, m_end;
    int m_startDir = 1, m_endDir = -1;
};

void DiagramTable::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *)
{
    static const QIcon keyIcon = Icons::get(QStringLiteral("primary_key_16.ico"));
    static const QIcon fkIcon = Icons::get(QStringLiteral("manrel_16.ico"));
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF r(QPointF(0, 0), m_size);
    QPainterPath box;
    box.addRoundedRect(r, 4, 4);
    p->fillPath(box, m_colors->box);
    p->save();
    p->setClipPath(box);
    p->fillRect(QRectF(0, 0, r.width(), m_headerHeight), m_colors->header);
    p->restore();
    p->setPen(QPen(isSelected() ? m_colors->highlight : m_colors->border, isSelected() ? 2 : 1));
    p->setBrush(Qt::NoBrush);
    p->drawPath(box);

    p->setFont(m_bold);
    p->setPen(m_colors->headerText);
    p->drawText(QRectF(8, 0, r.width() - 16, m_headerHeight), Qt::AlignVCenter | Qt::AlignLeft,
                m_title);
    for(int i = 0; i < m_table.columns.size(); ++i) {
        const SchemaDiagram::Column &c = m_table.columns[i];
        const QRectF row(kText, m_headerHeight + 3 + i * m_rowHeight, r.width() - kText - 8,
                         m_rowHeight);
        const QIcon *icon = c.primaryKey ? &keyIcon : c.foreignKey ? &fkIcon : nullptr;
        if(icon)
            icon->paint(p, QRect(6, int(row.top() + (m_rowHeight - 14) / 2), 14, 14));
        p->setFont(c.primaryKey ? m_bold : m_font);
        p->setPen(m_colors->text);
        p->drawText(row, Qt::AlignVCenter | Qt::AlignLeft, m_names[i]);
        p->setFont(m_font);
        p->setPen(m_colors->typeText);
        p->drawText(row, Qt::AlignVCenter | Qt::AlignRight, m_types[i]);
    }
}

QVariant DiagramTable::itemChange(GraphicsItemChange change, const QVariant &value)
{
    if(change == ItemPositionHasChanged) {
        for(DiagramLink *l : std::as_const(m_links))
            l->updatePath();
        /* room to keep dragging past the edge */
        if(QGraphicsScene *s = scene())
            s->setSceneRect(
                s->sceneRect().united(sceneBoundingRect().adjusted(-200, -200, 200, 200)));
    } else if(change == ItemSelectedHasChanged) {
        for(DiagramLink *l : std::as_const(m_links))
            l->update();
    }
    return QGraphicsItem::itemChange(change, value);
}

/* the canvas: drag the background to pan, Ctrl+wheel to zoom */
class DiagramView : public QGraphicsView
{
public:
    DiagramView(QGraphicsScene *scene, const Colors *colors, QWidget *parent)
        : QGraphicsView(scene, parent), m_colors(colors)
    {
        setDragMode(ScrollHandDrag);
        setTransformationAnchor(AnchorUnderMouse);
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                       QPainter::SmoothPixmapTransform);
        setViewportUpdateMode(BoundingRectViewportUpdate);
    }

    std::function<void()> zoomChanged;
    std::function<void()> resized;
    std::function<void()> userZoomed; /* Ctrl+wheel */
    std::function<void(const QPoint &)> contextMenu;
    std::function<void(DiagramTable *)> tableDoubleClicked;

    qreal zoom() const
    {
        return transform().m11();
    }
    void setZoom(qreal z)
    {
        z = std::clamp<qreal>(z, 0.1, 3.0);
        setTransform(QTransform::fromScale(z, z));
        if(zoomChanged)
            zoomChanged();
    }
    DiagramTable *tableAt(const QPoint &pos) const
    {
        for(QGraphicsItem *it : items(pos))
            if(auto *t = dynamic_cast<DiagramTable *>(it))
                return t;
        return nullptr;
    }

protected:
    void wheelEvent(QWheelEvent *e) override
    {
        if(e->modifiers() & Qt::ControlModifier) {
            setZoom(zoom() * (e->angleDelta().y() > 0 ? 1.15 : 1 / 1.15));
            if(userZoomed)
                userZoomed();
            e->accept();
            return;
        }
        QGraphicsView::wheelEvent(e);
    }
    void resizeEvent(QResizeEvent *e) override
    {
        QGraphicsView::resizeEvent(e);
        if(resized)
            resized();
    }
    void contextMenuEvent(QContextMenuEvent *e) override
    {
        if(contextMenu)
            contextMenu(e->pos());
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if(DiagramTable *t = tableAt(e->pos()); t && tableDoubleClicked) {
            tableDoubleClicked(t);
            return;
        }
        QGraphicsView::mouseDoubleClickEvent(e);
    }
    /* the canvas colour and a dot grid; not part of an exported image,
     * which renders the scene, not the view */
    void drawBackground(QPainter *p, const QRectF &rect) override
    {
        p->fillRect(rect, m_colors->canvas);
        if(zoom() < 0.5)
            return;
        QColor dot = m_colors->border;
        dot.setAlpha(90);
        p->setPen(QPen(dot, 0));
        const qreal step = 24;
        for(qreal x = std::floor(rect.left() / step) * step; x < rect.right(); x += step)
            for(qreal y = std::floor(rect.top() / step) * step; y < rect.bottom(); y += step)
                p->drawPoint(QPointF(x, y));
    }

private:
    const Colors *m_colors;
};

SchemaDesigner::SchemaDesigner(const QString &database, Loader loader, QWidget *parent)
    : QWidget(parent), m_database(database), m_loader(std::move(loader))
{
    readColors();
    m_scene = new QGraphicsScene(this);
    m_view = new DiagramView(m_scene, &m_colors, this);
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
    m_view->tableDoubleClicked = [this](DiagramTable *t) {
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
    const QPalette pal = palette();
    m_colors.canvas = pal.color(QPalette::AlternateBase);
    m_colors.box = pal.color(QPalette::Base);
    m_colors.border = pal.color(QPalette::Mid);
    m_colors.text = pal.color(QPalette::Text);
    m_colors.typeText = pal.color(QPalette::PlaceholderText);
    m_colors.header = QColor(0x3B, 0x7D, 0xBB); /* SQLyog's blue, as on the welcome page */
    m_colors.headerText = Qt::white;
    m_colors.line = pal.color(QPalette::Text);
    m_colors.line.setAlpha(150);
    m_colors.highlight = pal.color(QPalette::Highlight);
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
        auto *item = new DiagramTable(t, &m_colors, font());
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
        DiagramTable *from = m_items.value(l.table), *to = m_items.value(l.refTable);
        auto *link = new DiagramLink(l, from, to, &m_colors);
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
    const auto *current = sel.size() == 1 ? dynamic_cast<DiagramTable *>(sel.first()) : nullptr;
    const int at = current ? int(matches.indexOf(current->name())) : -1;
    DiagramTable *next = m_items.value(matches.value((at + 1) % matches.size()));
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
    if(DiagramTable *t = m_view->tableAt(viewPos)) {
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
    const QList<DiagramTable *> items = m_items.values();
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
