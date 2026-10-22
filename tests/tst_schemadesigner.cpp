/* OpenYog — unit tests for the Schema Designer's canvas, driven the way a
 * user drives it (the mouse, on an offscreen window): the arrangement is
 * kept in its layout file and comes back, tables new to the schema are set
 * beside the ones already placed, and a column dragged onto another table's
 * asks for a foreign key. */
#include "SchemaDesigner.h"

#include <QFile>
#include <QGraphicsView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

SchemaDiagram::Table table(const QString &name, const QStringList &columns)
{
    SchemaDiagram::Table t;
    t.name = name;
    for(const QString &c : columns)
        t.columns.append({c, QStringLiteral("int")});
    return t;
}

SchemaDiagram::Model model(bool withOrders = false)
{
    SchemaDiagram::Model m;
    m.tables << table("customers", {"id", "name"}) << table("invoices", {"id", "customer_id"});
    m.links.append({"fk_inv", "invoices", {"customer_id"}, "customers", {"id"}});
    if(withOrders)
        m.tables << table("orders", {"id", "invoice_id", "note"});
    return m;
}

Diagram::Table *item(SchemaDesigner &d, const QString &name)
{
    for(QGraphicsItem *it : d.findChild<QGraphicsView *>()->scene()->items())
        if(auto *t = dynamic_cast<Diagram::Table *>(it); t && t->name() == name)
            return t;
    return nullptr;
}

/* a point on `column`'s row of `table`, in the view's viewport */
QPoint at(SchemaDesigner &d, const QString &table, const QString &column)
{
    Diagram::Table *t = item(d, table);
    const qreal y = column.isEmpty() ? 4 : t->rowCenter(column);
    return d.findChild<QGraphicsView *>()->mapFromScene(
        t->mapToScene(QPointF(t->size().width() / 2, y)));
}

void drag(SchemaDesigner &d, QPoint from, QPoint to)
{
    QWidget *viewport = d.findChild<QGraphicsView *>()->viewport();
    QTest::mousePress(viewport, Qt::LeftButton, {}, from);
    QTest::mouseMove(viewport, (from + to) / 2);
    QTest::mouseMove(viewport, to);
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, to);
}

} // namespace

class TestSchemaDesigner : public QObject
{
    Q_OBJECT
private slots:
    void arrangementKept()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("diagrams/conn/shop.json");
        QPointF moved;
        {
            SchemaDesigner d("shop", [] { return model(); });
            d.setLayoutFile(file);
            d.resize(900, 600);
            d.show();
            QVERIFY(QTest::qWaitForWindowExposed(&d));
            d.reload();
            const QPointF before = item(d, "customers")->pos();
            drag(d, at(d, "customers", {}), at(d, "customers", {}) + QPoint(40, 60));
            moved = item(d, "customers")->pos();
            QVERIFY(moved != before); /* the title bar moves the table */
            QTRY_VERIFY(QFile::exists(file));
        }
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject positions =
            QJsonDocument::fromJson(f.readAll()).object().value("positions").toObject();
        QCOMPARE(positions.keys(), QStringList({"customers", "invoices"}));

        SchemaDesigner again("shop", [] { return model(true); });
        again.setLayoutFile(file);
        again.reload();
        QCOMPARE(item(again, "customers")->pos(), moved);
        /* the new table beside the placed ones, overlapping none */
        const qreal right =
            std::max(item(again, "customers")->pos().x() + item(again, "customers")->size().width(),
                     item(again, "invoices")->pos().x() + item(again, "invoices")->size().width());
        QVERIFY(item(again, "orders")->pos().x() >= right);
        QVERIFY(again.dumpForTest().value(0).endsWith("overlaps=0"));
    }

    void moveSavedOnClose()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("shop.json");
        {
            SchemaDesigner d("shop", [] { return model(); });
            d.setLayoutFile(file);
            d.reload();
            item(d, "invoices")->setPos(1234, 567);
            QVERIFY(!QFile::exists(file)); /* not yet: a moment after the last move */
        }
        SchemaDesigner again("shop", [] { return model(); });
        again.setLayoutFile(file);
        again.reload();
        QCOMPARE(item(again, "invoices")->pos(), QPointF(1234, 567));
    }

    void columnDraggedOntoAnother()
    {
        SchemaDesigner d("shop", [] { return model(true); });
        d.resize(900, 600);
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        d.reload();
        QSignalSpy drawn(&d, &SchemaDesigner::relationshipDrawn);
        const QPointF before = item(d, "orders")->pos();
        drag(d, at(d, "orders", "invoice_id"), at(d, "invoices", "id"));
        QTRY_COMPARE(drawn.size(), 1);
        QCOMPARE(drawn.first(), QVariantList({"shop", "orders", "invoice_id", "invoices", "id"}));
        QCOMPARE(item(d, "orders")->pos(), before); /* a column row doesn't move the table */

        /* dropped on the background, or on its own table: nothing */
        drag(d, at(d, "orders", "invoice_id"), at(d, "orders", "id"));
        drag(d, at(d, "orders", "invoice_id"), QPoint(2, 2));
        QTest::qWait(50);
        QCOMPARE(drawn.size(), 1);
    }
};

QTEST_MAIN(TestSchemaDesigner)
#include "tst_schemadesigner.moc"
