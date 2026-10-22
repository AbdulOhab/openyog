/* OpenYog — the Schema Designer's and Query Builder's canvas; see
 * DiagramItems.h. */
#include "DiagramItems.h"
#include "Icons.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QMimeData>
#include <QPainter>
#include <QPainterPathStroker>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace Diagram {

Colors colorsFrom(const QPalette &pal)
{
    Colors c;
    c.canvas = pal.color(QPalette::AlternateBase);
    c.box = pal.color(QPalette::Base);
    c.border = pal.color(QPalette::Mid);
    c.text = pal.color(QPalette::Text);
    c.typeText = pal.color(QPalette::PlaceholderText);
    c.header = QColor(0x3B, 0x7D, 0xBB); /* SQLyog's blue, as on the welcome page */
    c.headerText = Qt::white;
    c.line = pal.color(QPalette::Text);
    c.line.setAlpha(150);
    c.highlight = pal.color(QPalette::Highlight);
    return c;
}

namespace {
constexpr qreal kCheckX = 6; /* the checkbox, in checkable boxes */
constexpr qreal kCheckEnd = 22;
constexpr qreal kIconWidth = 18; /* the key icon and its gap */

void drawCheckBox(QPainter *p, const QRectF &r, bool on, const Colors &c)
{
    p->setPen(QPen(on ? c.highlight : c.border, 1));
    p->setBrush(on ? c.highlight : c.box);
    p->drawRoundedRect(r, 2, 2);
    if(!on)
        return;
    QPen tick(Qt::white, 1.6);
    tick.setCapStyle(Qt::RoundCap);
    tick.setJoinStyle(Qt::RoundJoin);
    p->setPen(tick);
    p->setBrush(Qt::NoBrush);
    QPainterPath path;
    path.moveTo(r.left() + 3, r.center().y());
    path.lineTo(r.left() + r.width() * 0.42, r.bottom() - 3);
    path.lineTo(r.right() - 3, r.top() + 3);
    p->drawPath(path);
}
} // namespace

Table::Table(const SchemaDiagram::Table &table, const Colors *colors, const QFont &font,
             const QString &title, bool checkable)
    : m_table(table), m_colors(colors), m_font(font), m_bold(font), m_checkable(checkable),
      m_textX(checkable ? kCheckEnd + 2 + kIconWidth : 24)
{
    m_bold.setBold(true);
    const QFontMetricsF fm(m_font), bm(m_bold);
    m_rowHeight = std::ceil(fm.height()) + 4;
    m_headerHeight = std::ceil(bm.height()) + 8;
    m_title = bm.elidedText(title.isEmpty() ? table.name : title, Qt::ElideRight, 320);
    qreal nameWidth = 0, typeWidth = 0;
    for(const SchemaDiagram::Column &c : table.columns) {
        const QFontMetricsF &m = c.primaryKey ? bm : fm;
        m_names << m.elidedText(c.name, Qt::ElideRight, 220);
        m_types << fm.elidedText(c.type, Qt::ElideRight, 150);
        nameWidth = std::max(nameWidth, m.horizontalAdvance(m_names.last()));
        typeWidth = std::max(typeWidth, fm.horizontalAdvance(m_types.last()));
    }
    const qreal width = std::max(
        {m_textX + nameWidth + 18 + typeWidth + 8, bm.horizontalAdvance(m_title) + 16, qreal(120)});
    m_size = QSizeF(std::ceil(width), m_headerHeight + 3 + table.columns.size() * m_rowHeight + 4);

    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setToolTip(QStringLiteral("%1 — %2 columns").arg(table.name).arg(table.columns.size()));
}

QRectF Table::boundingRect() const
{
    return QRectF(QPointF(-2, -2), m_size + QSizeF(4, 4));
}

qreal Table::rowCenter(const QString &column) const
{
    for(int i = 0; i < m_table.columns.size(); ++i)
        if(m_table.columns[i].name == column)
            return m_headerHeight + 3 + (i + 0.5) * m_rowHeight;
    return m_headerHeight / 2;
}

QString Table::columnAt(const QPointF &itemPos) const
{
    if(itemPos.x() < 0 || itemPos.x() > m_size.width())
        return {};
    const qreal y = itemPos.y() - m_headerHeight - 3;
    if(y < 0)
        return {};
    const int row = int(y / m_rowHeight);
    return row < m_table.columns.size() ? m_table.columns[row].name : QString();
}

void Table::setChecked(const QString &column, bool on)
{
    if(on == m_checked.contains(column))
        return;
    if(on)
        m_checked.insert(column);
    else
        m_checked.remove(column);
    update();
}

void Table::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *)
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
    const qreal iconX = m_textX - kIconWidth;
    for(int i = 0; i < m_table.columns.size(); ++i) {
        const SchemaDiagram::Column &c = m_table.columns[i];
        const QRectF row(m_textX, m_headerHeight + 3 + i * m_rowHeight, r.width() - m_textX - 8,
                         m_rowHeight);
        if(m_checkable) {
            drawCheckBox(p, QRectF(kCheckX, row.top() + (m_rowHeight - 13) / 2, 13, 13),
                         m_checked.contains(c.name), *m_colors);
        }
        const QIcon *icon = c.primaryKey ? &keyIcon : c.foreignKey ? &fkIcon : nullptr;
        if(icon)
            icon->paint(p, QRect(int(iconX), int(row.top() + (m_rowHeight - 14) / 2), 14, 14));
        p->setFont(c.primaryKey ? m_bold : m_font);
        p->setPen(m_colors->text);
        p->drawText(row, Qt::AlignVCenter | Qt::AlignLeft, m_names[i]);
        p->setFont(m_font);
        p->setPen(m_colors->typeText);
        p->drawText(row, Qt::AlignVCenter | Qt::AlignRight, m_types[i]);
    }
}

QVariant Table::itemChange(GraphicsItemChange change, const QVariant &value)
{
    if(change == ItemPositionHasChanged) {
        for(Link *l : std::as_const(m_links))
            l->updatePath();
        /* room to keep dragging past the edge */
        if(QGraphicsScene *s = scene())
            s->setSceneRect(
                s->sceneRect().united(sceneBoundingRect().adjusted(-200, -200, 200, 200)));
    } else if(change == ItemSelectedHasChanged) {
        for(Link *l : std::as_const(m_links))
            l->update();
    }
    return QGraphicsItem::itemChange(change, value);
}

/* checkable boxes: a click on a checkbox toggles it, a drag from a column
 * row draws a dashed line to wherever it is dropped; the title bar (and
 * every row of a plain box) still moves the table */
void Table::mousePressEvent(QGraphicsSceneMouseEvent *e)
{
    const QString column = m_checkable ? columnAt(e->pos()) : QString();
    if(column.isEmpty() || e->button() != Qt::LeftButton) {
        QGraphicsItem::mousePressEvent(e);
        return;
    }
    e->accept();
    if(e->pos().x() < kCheckEnd) {
        setChecked(column, !isChecked(column));
        if(toggled)
            toggled(column, isChecked(column));
        return;
    }
    m_dragColumn = column;
    m_dragLine = new QGraphicsLineItem(QLineF(e->scenePos(), e->scenePos()));
    QPen pen(m_colors->highlight, 1.5, Qt::DashLine);
    m_dragLine->setPen(pen);
    m_dragLine->setZValue(10);
    scene()->addItem(m_dragLine);
}

void Table::mouseMoveEvent(QGraphicsSceneMouseEvent *e)
{
    if(!m_dragLine) {
        QGraphicsItem::mouseMoveEvent(e);
        return;
    }
    m_dragLine->setLine(QLineF(m_dragLine->line().p1(), e->scenePos()));
}

void Table::mouseReleaseEvent(QGraphicsSceneMouseEvent *e)
{
    if(!m_dragLine) {
        QGraphicsItem::mouseReleaseEvent(e);
        return;
    }
    delete m_dragLine;
    m_dragLine = nullptr;
    const QString column = m_dragColumn;
    m_dragColumn.clear();
    for(QGraphicsItem *it : scene()->items(e->scenePos())) {
        auto *target = dynamic_cast<Table *>(it);
        if(!target)
            continue;
        const QString targetColumn = target->columnAt(target->mapFromScene(e->scenePos()));
        if(target != this && !targetColumn.isEmpty() && joinDropped)
            joinDropped(column, target, targetColumn);
        break;
    }
}

Link::Link(Table *from, const QString &fromColumn, Table *to, const QString &toColumn,
           const Colors *colors, Style style)
    : m_from(from), m_to(to), m_fromColumn(fromColumn), m_toColumn(toColumn), m_colors(colors),
      m_style(style)
{
    setZValue(-1); /* behind the tables */
    updatePath();
}

void Link::setLabel(const QString &label)
{
    prepareGeometryChange();
    m_label = label;
    update();
}

void Link::updatePath()
{
    const QRectF a(m_from->pos(), m_from->size()), b(m_to->pos(), m_to->size());
    const qreal ya = a.top() + m_from->rowCenter(m_fromColumn);
    const qreal yb = b.top() + m_to->rowCenter(m_toColumn);
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
        path.cubicTo(m_start + QPointF(m_startDir * bend, 0), m_end + QPointF(m_endDir * bend, 0),
                     m_end);
    }
    setPath(path);
}

namespace {
QFont labelFont()
{
    QFont f = QApplication::font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    f.setBold(true);
    return f;
}
} // namespace

QRectF Link::labelRect() const
{
    if(m_label.isEmpty())
        return {};
    const QFontMetricsF fm(labelFont());
    const QSizeF size(fm.horizontalAdvance(m_label) + 10, fm.height() + 2);
    const QPointF mid = path().pointAtPercent(0.5);
    return QRectF(mid - QPointF(size.width() / 2, size.height() / 2), size);
}

QRectF Link::boundingRect() const
{
    const QRectF r = path().boundingRect().adjusted(-14, -14, 14, 14);
    return m_label.isEmpty() ? r : r.united(labelRect().adjusted(-2, -2, 2, 2));
}

/* the line, and the label (right-clicked as often as the line itself) */
QPainterPath Link::shape() const
{
    QPainterPathStroker s;
    s.setWidth(8);
    QPainterPath shape = s.createStroke(path());
    if(!m_label.isEmpty())
        shape.addRect(labelRect());
    return shape;
}

void Link::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *)
{
    const bool hot = m_from->isSelected() || m_to->isSelected();
    QPen pen(hot ? m_colors->highlight : m_colors->line, hot ? 2 : 1.3);
    pen.setCapStyle(Qt::RoundCap);
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    p->drawPath(path());
    if(m_style == Style::ForeignKey) {
        const QPointF fork = m_start + QPointF(m_startDir * 12, 0);
        p->drawLine(fork, m_start + QPointF(0, -6));
        p->drawLine(fork, m_start + QPointF(0, 6));
        const QPointF bar = m_end + QPointF(m_endDir * 8, 0);
        p->drawLine(bar + QPointF(0, -6), bar + QPointF(0, 6));
        return;
    }
    p->setBrush(pen.color());
    p->drawEllipse(m_start, 3, 3);
    p->drawEllipse(m_end, 3, 3);
    if(m_label.isEmpty())
        return;
    const QRectF tag = labelRect();
    p->setPen(QPen(pen.color(), 1));
    p->setBrush(m_colors->box);
    p->drawRoundedRect(tag, 3, 3);
    p->setFont(labelFont());
    p->setPen(m_colors->text);
    p->drawText(tag, Qt::AlignCenter, m_label);
}

View::View(QGraphicsScene *scene, const Colors *colors, QWidget *parent)
    : QGraphicsView(scene, parent), m_colors(colors)
{
    setDragMode(ScrollHandDrag);
    setTransformationAnchor(AnchorUnderMouse);
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                   QPainter::SmoothPixmapTransform);
    setViewportUpdateMode(BoundingRectViewportUpdate);
}

void View::setZoom(qreal z)
{
    z = std::clamp<qreal>(z, 0.1, 3.0);
    setTransform(QTransform::fromScale(z, z));
    if(zoomChanged)
        zoomChanged();
}

Table *View::tableAt(const QPoint &pos) const
{
    for(QGraphicsItem *it : items(pos))
        if(auto *t = dynamic_cast<Table *>(it))
            return t;
    return nullptr;
}

Link *View::linkAt(const QPoint &pos) const
{
    for(QGraphicsItem *it : items(pos))
        if(auto *l = dynamic_cast<Link *>(it))
            return l;
    return nullptr;
}

void View::wheelEvent(QWheelEvent *e)
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

void View::resizeEvent(QResizeEvent *e)
{
    QGraphicsView::resizeEvent(e);
    if(resized)
        resized();
}

void View::contextMenuEvent(QContextMenuEvent *e)
{
    if(contextMenu)
        contextMenu(e->pos());
}

void View::mouseDoubleClickEvent(QMouseEvent *e)
{
    if(Table *t = tableAt(e->pos()); t && tableDoubleClicked) {
        tableDoubleClicked(t);
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(e);
}

void View::dragEnterEvent(QDragEnterEvent *e)
{
    if(textDropped && e->mimeData()->hasText())
        e->acceptProposedAction();
    else
        QGraphicsView::dragEnterEvent(e);
}

void View::dragMoveEvent(QDragMoveEvent *e)
{
    if(textDropped && e->mimeData()->hasText())
        e->acceptProposedAction();
    else
        QGraphicsView::dragMoveEvent(e);
}

void View::dropEvent(QDropEvent *e)
{
    if(!textDropped || !e->mimeData()->hasText()) {
        QGraphicsView::dropEvent(e);
        return;
    }
    e->acceptProposedAction();
    textDropped(e->mimeData()->text(), mapToScene(e->position().toPoint()));
}

/* the canvas colour and a dot grid; not part of an exported image, which
 * renders the scene, not the view */
void View::drawBackground(QPainter *p, const QRectF &rect)
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

} // namespace Diagram
