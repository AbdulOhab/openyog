/* OpenYog — the Schema Designer tab (File ▸ New Schema Designer): one
 * database's tables as boxes, the foreign keys between them as lines, on a
 * canvas that can be panned, zoomed, rearranged by dragging, searched and
 * saved as an image. It only reads the schema: changing a table goes through
 * the usual Alter Table / Foreign Keys dialogs, which it asks its
 * ConnectionTab to open (the signals below).
 *
 * The model and the automatic layout are SchemaDiagram's; this is the
 * drawing and the interaction. */
#pragma once

#include "DiagramItems.h"
#include "SchemaDiagram.h"

#include <QSet>
#include <QWidget>

#include <functional>

class QGraphicsScene;
class QLabel;
class QLineEdit;

class SchemaDesigner : public QWidget
{
    Q_OBJECT
public:
    /* reads the schema afresh; called on open and on Refresh */
    using Loader = std::function<SchemaDiagram::Model()>;

    SchemaDesigner(const QString &database, Loader loader, QWidget *parent = nullptr);

    QString database() const
    {
        return m_database;
    }
    /* re-reads the schema: tables keep where they were dragged unless the
     * set of tables changed, which lays the diagram out afresh */
    void reload();
    void autoArrange();
    bool exportImage(const QString &file);
    void promptExportImage();

    /* --schemadesigner selftest: one summary line, then one per link */
    QStringList dumpForTest() const;

signals:
    void openTableRequested(const QString &database, const QString &table);
    void alterTableRequested(const QString &database, const QString &table);
    void foreignKeysRequested(const QString &database, const QString &table);

protected:
    void changeEvent(QEvent *event) override;

private:
    void rebuild(bool keepPositions);
    void hideTable(const QString &table);
    void findNext();
    void fitToWindow();
    void updateInfo();
    void showContextMenu(const QPoint &viewPos);
    void readColors();

    QString m_database;
    Loader m_loader;
    SchemaDiagram::Model m_model;
    QSet<QString> m_hidden;
    QHash<QString, QPointF> m_positions; /* where each table was last, shown or not */
    int m_linkCount = 0;
    bool m_autoFit = true; /* refit on resize, until zoomed by hand */
    QHash<QString, Diagram::Table *> m_items;
    Diagram::Colors m_colors;

    QGraphicsScene *m_scene = nullptr;
    Diagram::View *m_view = nullptr;
    QLabel *m_info = nullptr;
    QLabel *m_zoom = nullptr;
    QLineEdit *m_find = nullptr;
};
