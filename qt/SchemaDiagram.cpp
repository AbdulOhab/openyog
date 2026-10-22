/* OpenYog — the Schema Designer's model and layout; see SchemaDiagram.h. */
#include "SchemaDiagram.h"
#include "db/IDbConnection.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>

namespace SchemaDiagram {

Model fromRows(const QStringList &tables, const QHash<QString, DbResultSet> &columns,
               const DbResultSet &foreignKeys)
{
    Model m;
    QHash<QString, int> index;
    for(const QString &name : tables) {
        Table t{name, {}};
        for(const QStringList &row : columns.value(name).rows)
            t.columns.append({row.value(0), row.value(1), row.value(3) == QStringLiteral("PRI"),
                              false, row.value(2) == QStringLiteral("NO")});
        index.insert(name, int(m.tables.size()));
        m.tables.append(t);
    }

    /* rows of one composite key are consecutive */
    for(const QStringList &row : foreignKeys.rows) {
        const QString &table = row.value(0), &name = row.value(1), &refTable = row.value(3);
        if(!index.contains(table) || !index.contains(refTable))
            continue; /* points outside this database's base tables */
        if(m.links.isEmpty() || m.links.last().table != table || m.links.last().name != name)
            m.links.append({name, table, {}, refTable, {}});
        m.links.last().columns << row.value(2);
        m.links.last().refColumns << row.value(4);
    }
    for(const Link &l : std::as_const(m.links))
        for(Column &c : m.tables[index.value(l.table)].columns)
            if(l.columns.contains(c.name))
                c.foreignKey = true;
    return m;
}

Model load(IDbConnection *conn, const QString &db)
{
    const QStringList tables = conn->listTables(db, QStringLiteral("BASE TABLE"));
    QHash<QString, DbResultSet> columns;
    for(const QString &t : tables)
        columns.insert(t, conn->listColumns(db, t));
    return fromRows(tables, columns, conn->listAllForeignKeys(db));
}

Table loadTable(IDbConnection *conn, const QString &db, const QString &table)
{
    Table t{table, {}};
    for(const QStringList &row : conn->listColumns(db, table).rows)
        t.columns.append({row.value(0), row.value(1), row.value(3) == QStringLiteral("PRI"), false,
                          row.value(2) == QStringLiteral("NO")});
    /* listForeignKeys: Name Column Ref_table Ref_column */
    QSet<QString> fkColumns;
    for(const QStringList &row : conn->listForeignKeys(db, table).rows)
        fkColumns.insert(row.value(1));
    for(Column &c : t.columns)
        c.foreignKey = fkColumns.contains(c.name);
    return t;
}

QList<Link> loadForeignKeys(IDbConnection *conn, const QString &db)
{
    return fromRows(conn->listTables(db, QStringLiteral("BASE TABLE")), {},
                    conn->listAllForeignKeys(db))
        .links;
}

namespace {

constexpr double kRankGap = 90;  /* between columns of tables: room for the lines */
constexpr double kRowGap = 28;   /* between tables stacked in one column */
constexpr double kGroupGap = 80; /* between separately laid-out groups */
/* a column taller than this wraps into several side by side */
constexpr double kMaxColumnHeight = 1400;

bool nameLess(const QString &a, const QString &b)
{
    const int c = QString::compare(a, b, Qt::CaseInsensitive);
    return c != 0 ? c < 0 : a < b;
}

struct Block
{
    QHash<QString, QPointF> pos; /* relative to the block's top-left */
    QSizeF size;
};

/* one group of tables joined by foreign keys, in columns by rank */
Block layoutGroup(const QStringList &names, const QHash<QString, QStringList> &parents,
                  const QHash<QString, QStringList> &children,
                  const std::function<QSizeF(const QString &)> &sizeOf)
{
    /* rank: 0 for a table that references nothing here, else one more than
     * the highest-ranked table it references; a cycle is cut where the
     * walk meets a table it is still inside */
    QHash<QString, int> rank;
    QSet<QString> onPath;
    std::function<int(const QString &)> rankOf = [&](const QString &t) -> int {
        if(rank.contains(t))
            return rank.value(t);
        onPath.insert(t);
        int r = 0;
        for(const QString &p : parents.value(t))
            if(!onPath.contains(p))
                r = std::max(r, rankOf(p) + 1);
        onPath.remove(t);
        rank.insert(t, r);
        return r;
    };
    QList<QStringList> ranks;
    for(const QString &t : names) {
        const int r = rankOf(t);
        while(ranks.size() <= r)
            ranks.append(QStringList());
        ranks[r].append(t);
    }

    /* fewer crossings: order each column by where its neighbours sit
     * (barycenter), sweeping right then left a few times */
    QHash<QString, double> place; /* position within its column, 0..1 */
    const auto renumber = [&](const QStringList &col) {
        for(int i = 0; i < col.size(); ++i)
            place.insert(col[i], col.size() == 1 ? 0.5 : double(i) / (col.size() - 1));
    };
    for(const QStringList &col : std::as_const(ranks))
        renumber(col);
    const auto reorder = [&](QStringList &col, const QHash<QString, QStringList> &neighbours) {
        QHash<QString, double> key;
        for(const QString &t : col) {
            double sum = 0;
            int n = 0;
            for(const QString &o : neighbours.value(t))
                if(o != t) {
                    sum += place.value(o);
                    ++n;
                }
            key.insert(t, n ? sum / n : place.value(t));
        }
        std::stable_sort(col.begin(), col.end(), [&](const QString &a, const QString &b) {
            return key.value(a) < key.value(b);
        });
        renumber(col);
    };
    for(int sweep = 0; sweep < 4; ++sweep) {
        for(int r = 1; r < ranks.size(); ++r)
            reorder(ranks[r], parents);
        for(int r = int(ranks.size()) - 2; r >= 0; --r)
            reorder(ranks[r], children);
    }

    /* columns, a too-tall one wrapped into several, each centred vertically */
    struct SubColumn
    {
        QStringList names;
        double width = 0, height = 0;
    };
    QList<SubColumn> cols;
    for(const QStringList &rankNames : std::as_const(ranks)) {
        double tallest = 0;
        for(const QString &t : rankNames)
            tallest = std::max(tallest, sizeOf(t).height());
        const double limit = std::max(kMaxColumnHeight, tallest);
        SubColumn cur;
        for(const QString &t : rankNames) {
            const QSizeF s = sizeOf(t);
            const double h = cur.names.isEmpty() ? s.height() : cur.height + kRowGap + s.height();
            if(!cur.names.isEmpty() && h > limit) {
                cols.append(cur);
                cur = SubColumn();
            }
            cur.height = cur.names.isEmpty() ? s.height() : cur.height + kRowGap + s.height();
            cur.width = std::max(cur.width, s.width());
            cur.names.append(t);
        }
        cols.append(cur);
    }
    double height = 0;
    for(const SubColumn &c : std::as_const(cols))
        height = std::max(height, c.height);

    Block b;
    double x = 0;
    for(const SubColumn &c : std::as_const(cols)) {
        double y = (height - c.height) / 2;
        for(const QString &t : c.names) {
            b.pos.insert(t, QPointF(x, y));
            y += sizeOf(t).height() + kRowGap;
        }
        x += c.width + kRankGap;
    }
    b.size = QSizeF(x - kRankGap, height);
    return b;
}

} // namespace

QHash<QString, QPointF> layout(const Model &model, const QHash<QString, QSizeF> &sizes)
{
    const auto sizeOf = [&](const QString &t) { return sizes.value(t, QSizeF(160, 60)); };

    QHash<QString, QStringList> parents, children;
    for(const Link &l : model.links)
        if(l.table != l.refTable) {
            if(!parents[l.table].contains(l.refTable))
                parents[l.table].append(l.refTable);
            if(!children[l.refTable].contains(l.table))
                children[l.refTable].append(l.table);
        }

    /* groups: tables joined by foreign keys either way */
    QStringList names;
    for(const Table &t : model.tables)
        names << t.name;
    std::sort(names.begin(), names.end(), nameLess);
    QHash<QString, int> group;
    QList<QStringList> groups;
    for(const QString &start : std::as_const(names)) {
        if(group.contains(start))
            continue;
        QStringList members;
        QStringList todo{start};
        group.insert(start, int(groups.size()));
        while(!todo.isEmpty()) {
            const QString t = todo.takeFirst();
            members << t;
            for(const QString &n : parents.value(t) + children.value(t))
                if(!group.contains(n)) {
                    group.insert(n, int(groups.size()));
                    todo << n;
                }
        }
        std::sort(members.begin(), members.end(), nameLess);
        groups.append(members);
    }
    /* the biggest groups first, then the tables with no foreign keys */
    std::stable_sort(groups.begin(), groups.end(), [](const QStringList &a, const QStringList &b) {
        return a.size() > b.size();
    });

    QList<Block> blocks;
    double area = 0, widest = 0;
    for(const QStringList &g : std::as_const(groups)) {
        blocks.append(layoutGroup(g, parents, children, sizeOf));
        area += blocks.last().size.width() * blocks.last().size.height();
        widest = std::max(widest, blocks.last().size.width());
    }

    /* rows of blocks, roughly as wide as the whole is tall; the lone tables
     * start a row of their own */
    const double rowWidth = std::max(widest, std::sqrt(area) * 1.6);
    QHash<QString, QPointF> out;
    double x = 0, y = 0, rowHeight = 0;
    for(int i = 0; i < blocks.size(); ++i) {
        const Block &b = blocks[i];
        const bool firstLone = groups[i].size() == 1 && i > 0 && groups[i - 1].size() > 1;
        if(x > 0 && (x + b.size.width() > rowWidth || firstLone)) {
            x = 0;
            y += rowHeight + kGroupGap;
            rowHeight = 0;
        }
        for(auto it = b.pos.cbegin(); it != b.pos.cend(); ++it)
            out.insert(it.key(), it.value() + QPointF(x, y));
        x += b.size.width() + kGroupGap;
        rowHeight = std::max(rowHeight, b.size.height());
    }
    return out;
}

} // namespace SchemaDiagram
