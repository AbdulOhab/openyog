/* OpenYog — unit tests for qt/SchemaDiagram: the Schema Designer's model
 * (tables, columns, foreign keys from IDbConnection's row shapes) and its
 * layout. */
#include "SchemaDiagram.h"
#include "db/IDbConnection.h"

#include <QRectF>
#include <QTest>

using namespace SchemaDiagram;

namespace {

/* SHOW COLUMNS shape: Field Type Null Key Default Extra Comment */
QStringList col(const QString &name, const QString &type, bool pk = false, bool notNull = false)
{
    return {name, type, notNull ? "NO" : "YES", pk ? "PRI" : "", "NULL", "", ""};
}

DbResultSet rows(const QList<QStringList> &r)
{
    DbResultSet rs;
    rs.rows = r;
    return rs;
}

/* a table per name, each referencing the tables `refs` maps it to */
Model graph(const QStringList &tables, const QList<QPair<QString, QString>> &refs)
{
    Model m;
    for(const QString &t : tables)
        m.tables.append({t, {{"id", "int", true, false, true}}});
    for(const auto &r : refs)
        m.links.append({"fk_" + r.first + "_" + r.second, r.first, {"ref_id"}, r.second, {"id"}});
    return m;
}

QHash<QString, QSizeF> sizesFor(const Model &m, double w = 160, double h = 80)
{
    QHash<QString, QSizeF> s;
    for(const Table &t : m.tables)
        s.insert(t.name, QSizeF(w + (t.name.size() % 3) * 20, h + (t.name.size() % 4) * 30));
    return s;
}

/* every table placed, and no two overlapping */
void checkPlacement(const Model &m, const QHash<QString, QSizeF> &sizes,
                    const QHash<QString, QPointF> &pos)
{
    QCOMPARE(pos.size(), m.tables.size());
    QList<QPair<QString, QRectF>> rects;
    for(const Table &t : m.tables) {
        QVERIFY2(pos.contains(t.name), qPrintable(t.name));
        rects.append({t.name, QRectF(pos.value(t.name), sizes.value(t.name))});
    }
    for(int i = 0; i < rects.size(); ++i)
        for(int j = i + 1; j < rects.size(); ++j)
            QVERIFY2(!rects[i].second.intersects(rects[j].second),
                     qPrintable(rects[i].first + " overlaps " + rects[j].first));
}

} // namespace

class TestSchemaDiagram : public QObject
{
    Q_OBJECT
private slots:
    void columnsAndKeysFromRows()
    {
        QHash<QString, DbResultSet> cols;
        cols.insert("emp_dept", rows({col("id", "int", true, true), col("name", "varchar(50)")}));
        cols.insert("employees", rows({col("id", "int", true, true), col("dept_id", "int"),
                                       col("boss_id", "int")}));
        cols.insert("orders", rows({col("id", "int", true, true), col("emp_id", "int"),
                                    col("dept_id", "int")}));
        /* listAllForeignKeys: table, constraint, column, ref table, ref column */
        const DbResultSet fks = rows({
            {"employees", "fk_boss", "boss_id", "employees", "id"},
            {"employees", "fk_emp_dept", "dept_id", "emp_dept", "id"},
            {"employees", "fk_elsewhere", "dept_id", "archive_dept", "id"},
            {"orders", "fk_order_emp", "emp_id", "employees", "id"},
            {"orders", "fk_order_emp", "dept_id", "employees", "dept_id"},
        });
        const Model m = fromRows({"emp_dept", "employees", "orders"}, cols, fks);

        QCOMPARE(m.tables.size(), 3);
        QCOMPARE(m.tables[0].name, QStringLiteral("emp_dept"));
        QCOMPARE(m.tables[0].columns.size(), 2);
        QVERIFY(m.tables[0].columns[0].primaryKey);
        QVERIFY(m.tables[0].columns[0].notNull);
        QVERIFY(!m.tables[0].columns[1].primaryKey);
        QCOMPARE(m.tables[0].columns[1].type, QStringLiteral("varchar(50)"));

        /* the key to a table outside the list is dropped; the composite
         * key's two rows are one link */
        QCOMPARE(m.links.size(), 3);
        QCOMPARE(m.links[0].name, QStringLiteral("fk_boss"));
        QCOMPARE(m.links[0].refTable, QStringLiteral("employees"));
        QCOMPARE(m.links[2].name, QStringLiteral("fk_order_emp"));
        QCOMPARE(m.links[2].columns, QStringList({"emp_id", "dept_id"}));
        QCOMPARE(m.links[2].refColumns, QStringList({"id", "dept_id"}));

        const Table &emp = m.tables[1];
        QVERIFY(!emp.columns[0].foreignKey);
        QVERIFY(emp.columns[1].foreignKey);
        QVERIFY(emp.columns[2].foreignKey);
        QVERIFY(m.tables[2].columns[1].foreignKey && m.tables[2].columns[2].foreignKey);
    }

    void referencedTablesSitLeft()
    {
        /* orders -> customers -> regions, orders -> products */
        const Model m =
            graph({"orders", "customers", "regions", "products"},
                  {{"orders", "customers"}, {"customers", "regions"}, {"orders", "products"}});
        const auto sizes = sizesFor(m);
        const auto pos = layout(m, sizes);
        checkPlacement(m, sizes, pos);
        const auto right = [&](const QString &t) { return pos[t].x() + sizes[t].width(); };
        QVERIFY(right("regions") <= pos["customers"].x());
        QVERIFY(right("customers") <= pos["orders"].x());
        QVERIFY(right("products") <= pos["orders"].x());
    }

    void sameSchemaSameLayout()
    {
        const QList<QPair<QString, QString>> refs = {
            {"b", "a"}, {"c", "a"}, {"d", "b"}, {"e", "c"}, {"e", "b"}};
        const Model m = graph({"a", "b", "c", "d", "e", "lone1", "lone2"}, refs);
        const Model shuffled = graph({"lone2", "e", "c", "a", "lone1", "d", "b"}, refs);
        const auto sizes = sizesFor(m);
        const auto pos = layout(m, sizes);
        checkPlacement(m, sizes, pos);
        QCOMPARE(layout(m, sizes), pos);
        QCOMPARE(layout(shuffled, sizes), pos);
    }

    void cyclesAndSelfReferences()
    {
        Model m = graph({"a", "b", "c"}, {{"a", "b"}, {"b", "a"}, {"c", "c"}, {"c", "a"}});
        const auto sizes = sizesFor(m);
        const auto pos = layout(m, sizes);
        checkPlacement(m, sizes, pos);
    }

    void tallColumnWraps()
    {
        /* forty tables all referencing one: one column would be ~9000px */
        QStringList names{"hub"};
        QList<QPair<QString, QString>> refs;
        for(int i = 0; i < 40; ++i) {
            names << QStringLiteral("child%1").arg(i, 2, 10, QLatin1Char('0'));
            refs.append({names.last(), "hub"});
        }
        for(int i = 0; i < 12; ++i)
            names << QStringLiteral("lone%1").arg(i);
        const Model m = graph(names, refs);
        QHash<QString, QSizeF> sizes;
        for(const QString &n : names)
            sizes.insert(n, QSizeF(180, 200));
        const auto pos = layout(m, sizes);
        checkPlacement(m, sizes, pos);

        QSet<double> childColumns;
        double bottom = 0;
        for(int i = 0; i < 40; ++i) {
            const QString n = names[i + 1];
            childColumns.insert(pos[n].x());
            bottom = std::max(bottom, pos[n].y() + 200);
            QVERIFY(pos["hub"].x() + 180 <= pos[n].x());
        }
        QVERIFY(childColumns.size() > 1);
        QVERIFY(bottom <= 1400 + 1);
        /* the lone tables come after the group, not beside its columns */
        for(int i = 0; i < 12; ++i)
            QVERIFY(pos[QStringLiteral("lone%1").arg(i)].y() >= bottom);
    }

    void emptySchema()
    {
        QVERIFY(layout(Model(), {}).isEmpty());
        QVERIFY(fromRows({}, {}, DbResultSet()).tables.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSchemaDiagram)
#include "tst_schemadiagram.moc"
