/* OpenYog — the canvas the Schema Designer and the Query Builder draw on: a
 * box per table (a title bar, then a row per column), lines between two
 * tables' columns, and a view that pans, zooms and takes drops.
 *
 * The two differ in a few switches. The Query Builder's boxes have a
 * checkbox in front of each column and let a column be dragged onto another
 * table's to ask for a join between them; its lines are joins, labelled with
 * their type, where the Schema Designer's are foreign keys with a crow's foot
 * at the referencing end. */
#pragma once

#include "SchemaDiagram.h"

#include <QColor>
#include <QFont>
#include <QGraphicsPathItem>
#include <QGraphicsView>
#include <QSet>

#include <functional>

class QGraphicsLineItem;

namespace Diagram {

/* from the palette, so every theme draws in its own colours */
struct Colors
{
    QColor canvas, box, border, text, typeText, header, headerText, line, highlight;
};
Colors colorsFrom(const QPalette &palette);

class Link;

class Table : public QGraphicsItem
{
public:
    /* `title`: what the title bar says, the table's name when empty;
     * `checkable`: the Query Builder's checkboxes and column dragging */
    Table(const SchemaDiagram::Table &table, const Colors *colors, const QFont &font,
          const QString &title = {}, bool checkable = false);

    QRectF boundingRect() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override;

    QSizeF size() const
    {
        return m_size;
    }
    const QString &name() const
    {
        return m_table.name;
    }
    /* a column's row, as a y in the item; the title bar for an unknown one */
    qreal rowCenter(const QString &column) const;
    /* the column whose row `itemPos` is on, or an empty string */
    QString columnAt(const QPointF &itemPos) const;
    void addLink(Link *link)
    {
        m_links << link;
    }

    bool isChecked(const QString &column) const
    {
        return m_checked.contains(column);
    }
    void setChecked(const QString &column, bool on); /* doesn't call toggled */

    /* a column row dragged onto another table's starts joinDropped, the
     * title bar still moving the table: on in checkable boxes, and the
     * Schema Designer's (a dragged relationship) */
    void setColumnDrag(bool on)
    {
        m_columnDrag = on;
    }

    /* a checkbox clicked */
    std::function<void(const QString &column, bool checked)> toggled;
    /* the box moved (dragged, or put somewhere) */
    std::function<void()> moved;
    /* a column dragged onto another table's column */
    std::function<void(const QString &column, Table *target, const QString &targetColumn)>
        joinDropped;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *e) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *e) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *e) override;

private:
    SchemaDiagram::Table m_table;
    const Colors *m_colors;
    QFont m_font, m_bold;
    bool m_checkable;
    bool m_columnDrag = false;
    qreal m_textX; /* where column names start, after the checkbox and key icon */
    QString m_title;
    QStringList m_names, m_types; /* elided to fit */
    qreal m_rowHeight = 0, m_headerHeight = 0;
    QSizeF m_size;
    QList<Link *> m_links;
    QSet<QString> m_checked;
    QString m_dragColumn;
    QGraphicsLineItem *m_dragLine = nullptr;
};

/* a curve from one table's column row to another's */
class Link : public QGraphicsPathItem
{
public:
    enum class Style {
        ForeignKey, /* crow's foot at `from` (the "many" end), a bar at `to` */
        Join,       /* a dot at each end, and the label half way */
    };
    Link(Table *from, const QString &fromColumn, Table *to, const QString &toColumn,
         const Colors *colors, Style style);

    void setLabel(const QString &label);
    void updatePath();
    Table *from() const
    {
        return m_from;
    }
    Table *to() const
    {
        return m_to;
    }

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override;

private:
    QRectF labelRect() const; /* empty without a label */

    Table *m_from, *m_to;
    QString m_fromColumn, m_toColumn;
    const Colors *m_colors;
    Style m_style;
    QString m_label;
    QPointF m_start, m_end;
    int m_startDir = 1, m_endDir = -1;
};

/* drag the background to pan, Ctrl+wheel to zoom */
class View : public QGraphicsView
{
public:
    View(QGraphicsScene *scene, const Colors *colors, QWidget *parent);

    std::function<void()> zoomChanged;
    std::function<void()> resized;
    std::function<void()> userZoomed; /* Ctrl+wheel */
    std::function<void(const QPoint &)> contextMenu;
    std::function<void(Table *)> tableDoubleClicked;
    /* text dropped on the canvas: a table name dragged from a list */
    std::function<void(const QString &text, const QPointF &scenePos)> textDropped;

    qreal zoom() const
    {
        return transform().m11();
    }
    void setZoom(qreal z);
    Table *tableAt(const QPoint &pos) const;
    Link *linkAt(const QPoint &pos) const;

protected:
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void drawBackground(QPainter *p, const QRectF &rect) override;

private:
    const Colors *m_colors;
};

} // namespace Diagram
