/* OpenYog — the Schema Designer tab (File ▸ New Schema Designer): one
 * database's tables as boxes, the foreign keys between them as lines, on a
 * canvas that can be panned, zoomed, rearranged by dragging, searched and
 * saved as an image; the arrangement is kept in a layout file. It never
 * changes the schema itself: altering or creating a table, or the foreign key
 * a column dragged onto another table's asks for, goes through its
 * ConnectionTab (the signals below), and the diagram is read again after.
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
class QTimer;

class SchemaDesigner : public QWidget
{
    Q_OBJECT
public:
    /* reads the schema afresh; called on open and on Refresh */
    using Loader = std::function<SchemaDiagram::Model()>;

    SchemaDesigner(const QString &database, Loader loader, QWidget *parent = nullptr);
    ~SchemaDesigner() override; /* a move not saved yet is saved */

    QString database() const
    {
        return m_database;
    }
    /* where the arrangement is kept between sessions (positions, hidden
     * tables), read on the first reload(); none: not kept */
    void setLayoutFile(const QString &path)
    {
        m_layoutFile = path;
    }
    /* re-reads the schema: tables keep where they were put; tables new to
     * the canvas are laid out beside them */
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
    void createTableRequested(const QString &database);
    /* a column dragged onto another table's: `table`.`column` to reference
     * `refTable`.`refColumn` */
    void relationshipDrawn(const QString &database, const QString &table, const QString &column,
                           const QString &refTable, const QString &refColumn);

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
    void loadLayout();
    void saveLayout();
    /* a dialog that may change the schema, then the schema read again */
    void thenReload(const std::function<void()> &request);

    QString m_database;
    Loader m_loader;
    SchemaDiagram::Model m_model;
    QSet<QString> m_hidden;
    QHash<QString, QPointF> m_positions; /* where each table was last, shown or not */
    int m_linkCount = 0;
    QString m_layoutFile;
    bool m_layoutLoaded = false;
    QTimer *m_saveTimer = nullptr;
    bool m_autoFit = true; /* refit on resize, until zoomed by hand */
    QHash<QString, Diagram::Table *> m_items;
    Diagram::Colors m_colors;

    QGraphicsScene *m_scene = nullptr;
    Diagram::View *m_view = nullptr;
    QLabel *m_info = nullptr;
    QLabel *m_zoom = nullptr;
    QLineEdit *m_find = nullptr;
};
