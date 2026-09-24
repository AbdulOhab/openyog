/* OpenYog — small shared bits for the flat, icon-only tool buttons of the
 * Form view and the Info tab's search strip: arrows painted in the palette's
 * own colours (crisp on every theme, greyed when disabled) and the one style
 * sheet that gives such buttons their hover frame. */
#pragma once

#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

namespace NavIcons {
enum Kind { First, Prev, Next, Last, Up, Down };

inline QIcon arrow(Kind kind, const QPalette &pal)
{
    QIcon icon;
    const struct
    {
        QIcon::Mode mode;
        QColor color;
    } modes[] = {{QIcon::Normal, pal.color(QPalette::Active, QPalette::ButtonText)},
                 {QIcon::Disabled, pal.color(QPalette::Disabled, QPalette::ButtonText)}};
    for(const auto &m : modes) {
        QPixmap pm(32, 32);
        pm.setDevicePixelRatio(2);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(m.color);
        const auto tri = [&](qreal tipX, qreal baseX) {
            const QPointF pts[3] = {{baseX, 3.5}, {tipX, 8.0}, {baseX, 12.5}};
            p.drawPolygon(pts, 3);
        };
        switch(kind) {
            case First: /* bar + left triangle */
                p.drawRoundedRect(QRectF(2.6, 3.5, 1.9, 9.0), 0.6, 0.6);
                tri(5.6, 12.2);
                break;
            case Prev:
                tri(4.6, 11.4);
                break;
            case Next:
                tri(11.4, 4.6);
                break;
            case Last:
                p.drawRoundedRect(QRectF(11.5, 3.5, 1.9, 9.0), 0.6, 0.6);
                tri(10.4, 3.8);
                break;
            case Up: {
                const QPointF pts[3] = {{3.5, 10.6}, {8.0, 5.0}, {12.5, 10.6}};
                p.drawPolygon(pts, 3);
                break;
            }
            case Down: {
                const QPointF pts[3] = {{3.5, 5.4}, {8.0, 11.0}, {12.5, 5.4}};
                p.drawPolygon(pts, 3);
                break;
            }
        }
        icon.addPixmap(pm, m.mode);
    }
    return icon;
}

/* flat until hovered, then a soft frame; translucent greys suit light and dark.
 * Buttons opt in by objectName "flatTool" (icon-only) or "flatAction" (icon+text). */
inline QString flatToolSheet()
{
    return QStringLiteral(
        "QToolButton#flatTool, QToolButton#flatAction {"
        " border: 1px solid transparent; border-radius: 4px; padding: 2px 8px; }"
        "QToolButton#flatTool { padding: 2px; }"
        "QToolButton#flatTool:hover:enabled, QToolButton#flatAction:hover:enabled {"
        " background: rgba(127, 127, 127, 45); border-color: rgba(127, 127, 127, 110); }"
        "QToolButton#flatTool:pressed:enabled, QToolButton#flatAction:pressed:enabled,"
        "QToolButton#flatTool:checked { background: rgba(127, 127, 127, 90);"
        " border-color: rgba(127, 127, 127, 140); }"
        "QToolButton#flatAction[danger=\"true\"]:hover:enabled {"
        " background: rgba(220, 70, 70, 60); border-color: rgba(220, 70, 70, 140); }");
}
} // namespace NavIcons
