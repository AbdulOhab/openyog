/* OpenYog — the Query Builder's SQL; see QueryBuild.h. */
#include "QueryBuild.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

namespace QueryBuild {

QString condition(const QString &expression, const QString &criteria)
{
    const QString c = criteria.trimmed();
    if(c.isEmpty())
        return {};
    static const QRegularExpression startsWithOperator(
        QStringLiteral(
            "^(<>|!=|<=|>=|=|<|>|(IS|LIKE|ILIKE|NOT|IN|BETWEEN|REGEXP|RLIKE|SIMILAR)\\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return startsWithOperator.match(c).hasMatch() ? expression + QLatin1Char(' ') + c
                                                  : expression + QStringLiteral(" = ") + c;
}

QString toSql(const Query &q, const Quote &quote)
{
    const int n = int(q.sources.size());
    if(n == 0)
        return {};
    const auto ref = [&](int s) { return quote(q.sources[s].ref()); };
    const auto qualified = [&](const Column &c) {
        return ref(c.source) + QLatin1Char('.') + quote(c.column);
    };
    const auto expression = [&](const Column &c) {
        return c.aggregate.isEmpty()
                   ? qualified(c)
                   : c.aggregate.toUpper() + QLatin1Char('(') + qualified(c) + QLatin1Char(')');
    };
    const auto sourceSql = [&](int s) {
        const Source &src = q.sources[s];
        const QString table = q.database.isEmpty()
                                  ? quote(src.table)
                                  : quote(q.database) + QLatin1Char('.') + quote(src.table);
        return src.alias.isEmpty() || src.alias == src.table
                   ? table
                   : table + QStringLiteral(" AS ") + quote(src.alias);
    };

    /* SELECT, and GROUP BY when anything is aggregated */
    QStringList select, groupBy;
    bool aggregated = false;
    for(const Column &c : q.columns) {
        if(!c.output)
            continue;
        const QString e = expression(c);
        select << (c.alias.isEmpty() ? e : e + QStringLiteral(" AS ") + quote(c.alias));
        if(c.aggregate.isEmpty())
            groupBy << qualified(c);
        else
            aggregated = true;
    }
    if(!aggregated)
        groupBy.clear();
    QString sql = QStringLiteral("SELECT");
    if(q.distinct)
        sql += QStringLiteral(" DISTINCT");
    sql += select.isEmpty() ? QStringLiteral(" *")
                            : QStringLiteral("\n    ") + select.join(QStringLiteral(",\n    "));

    /* FROM the first source; then, in the order they were added, each source
     * that joins one already placed, on every condition it has with those */
    sql += QStringLiteral("\nFROM ") + sourceSql(0);
    QList<bool> placed(n, false);
    placed[0] = true;
    for(int done = 1; done < n; ++done) {
        int next = -1;
        for(int s = 1; s < n && next < 0; ++s) {
            if(placed[s])
                continue;
            for(const Join &j : q.joins)
                if((j.left == s && placed.value(j.right)) ||
                   (j.right == s && placed.value(j.left))) {
                    next = s;
                    break;
                }
        }
        if(next < 0) {
            next = int(placed.indexOf(false));
            sql += QStringLiteral("\n    CROSS JOIN ") + sourceSql(next);
            placed[next] = true;
            continue;
        }
        QStringList on;
        JoinType type = JoinType::Inner;
        bool typed = false;
        for(const Join &j : q.joins) {
            bool nextIsLeft;
            int other;
            if(j.left == next && placed.value(j.right)) {
                nextIsLeft = true;
                other = j.right;
            } else if(j.right == next && placed.value(j.left)) {
                nextIsLeft = false;
                other = j.left;
            } else {
                continue;
            }
            for(const auto &pair : j.on)
                on << ref(next) + QLatin1Char('.') + quote(nextIsLeft ? pair.first : pair.second) +
                          QStringLiteral(" = ") + ref(other) + QLatin1Char('.') +
                          quote(nextIsLeft ? pair.second : pair.first);
            /* written "placed … JOIN next": an outer join keeping the placed
             * side is LEFT, one keeping `next` is RIGHT; the first join to
             * `next` decides */
            JoinType rel = JoinType::Inner;
            if(j.type != JoinType::Inner)
                rel = (j.type == JoinType::Left) == !nextIsLeft ? JoinType::Left : JoinType::Right;
            if(!typed) {
                type = rel;
                typed = true;
            }
        }
        static const char *const kKeyword[] = {"INNER JOIN", "LEFT JOIN", "RIGHT JOIN"};
        sql += QStringLiteral("\n    ") + QLatin1String(kKeyword[int(type)]) + QLatin1Char(' ') +
               sourceSql(next) + QStringLiteral(" ON ") + on.join(QStringLiteral(" AND "));
        placed[next] = true;
    }

    /* per set (Criteria, Or): its plain conditions and its aggregate ones */
    QStringList plain[2], aggregate[2];
    for(const Column &c : q.columns)
        for(int set = 0; set < 2; ++set) {
            const QString cond = condition(expression(c), set ? c.orCriteria : c.criteria);
            if(!cond.isEmpty())
                (c.aggregate.isEmpty() ? plain : aggregate)[set] << cond;
        }
    const bool used[2] = {!plain[0].isEmpty() || !aggregate[0].isEmpty(),
                          !plain[1].isEmpty() || !aggregate[1].isEmpty()};
    const auto either = [](const QStringList &a, const QStringList &b) {
        const auto set = [](const QStringList &s) {
            return s.size() > 1
                       ? QLatin1Char('(') + s.join(QStringLiteral(" AND ")) + QLatin1Char(')')
                       : s.value(0);
        };
        return set(a) + QStringLiteral("\n    OR ") + set(b);
    };
    QString where, having;
    if(!used[0] || !used[1]) {
        const int set = used[0] ? 0 : 1;
        where = plain[set].join(QStringLiteral("\n    AND "));
        having = aggregate[set].join(QStringLiteral("\n    AND "));
    } else if(aggregate[0].isEmpty() && aggregate[1].isEmpty()) {
        where = either(plain[0], plain[1]);
    } else if(plain[0].isEmpty() && plain[1].isEmpty()) {
        having = either(aggregate[0], aggregate[1]);
    } else {
        having = either(plain[0] + aggregate[0], plain[1] + aggregate[1]);
    }
    if(!where.isEmpty())
        sql += QStringLiteral("\nWHERE ") + where;
    if(!groupBy.isEmpty())
        sql += QStringLiteral("\nGROUP BY ") + groupBy.join(QStringLiteral(", "));
    if(!having.isEmpty())
        sql += QStringLiteral("\nHAVING ") + having;

    QStringList order;
    for(const Column &c : q.columns) {
        if(c.sort == Sort::None)
            continue;
        const QString e = c.output && !c.alias.isEmpty() ? quote(c.alias) : expression(c);
        order << (c.sort == Sort::Desc ? e + QStringLiteral(" DESC") : e);
    }
    if(!order.isEmpty())
        sql += QStringLiteral("\nORDER BY ") + order.join(QStringLiteral(", "));
    if(q.limit > 0)
        sql += QStringLiteral("\nLIMIT ") + QString::number(q.limit);
    return sql;
}

namespace {

const char *const kJoinTypes[] = {"inner", "left", "right"};
const char *const kSorts[] = {"", "asc", "desc"};
const char *const kFormat = "OpenYog Query Builder";

template<size_t N> int indexOf(const char *const (&names)[N], const QString &name)
{
    for(size_t i = 0; i < N; ++i)
        if(name == QLatin1String(names[i]))
            return int(i);
    return 0;
}

} // namespace

QJsonObject toJson(const Query &q)
{
    QJsonArray sources, joins, columns;
    for(const Source &s : q.sources)
        sources.append(
            QJsonObject{{QStringLiteral("table"), s.table}, {QStringLiteral("alias"), s.alias}});
    for(const Join &j : q.joins) {
        QJsonArray on;
        for(const auto &pair : j.on)
            on.append(QJsonArray{pair.first, pair.second});
        joins.append(QJsonObject{{QStringLiteral("left"), j.left},
                                 {QStringLiteral("right"), j.right},
                                 {QStringLiteral("type"), kJoinTypes[int(j.type)]},
                                 {QStringLiteral("on"), on}});
    }
    for(const Column &c : q.columns)
        columns.append(QJsonObject{{QStringLiteral("source"), c.source},
                                   {QStringLiteral("column"), c.column},
                                   {QStringLiteral("alias"), c.alias},
                                   {QStringLiteral("aggregate"), c.aggregate},
                                   {QStringLiteral("sort"), kSorts[int(c.sort)]},
                                   {QStringLiteral("criteria"), c.criteria},
                                   {QStringLiteral("or"), c.orCriteria},
                                   {QStringLiteral("output"), c.output}});
    return QJsonObject{
        {QStringLiteral("format"), kFormat},      {QStringLiteral("version"), 1},
        {QStringLiteral("database"), q.database}, {QStringLiteral("sources"), sources},
        {QStringLiteral("joins"), joins},         {QStringLiteral("columns"), columns},
        {QStringLiteral("distinct"), q.distinct}, {QStringLiteral("limit"), q.limit}};
}

bool fromJson(const QJsonObject &json, Query *q, QString *error)
{
    if(json.value(QStringLiteral("format")).toString() != QLatin1String(kFormat)) {
        if(error)
            *error = QStringLiteral("not a saved Query Builder query");
        return false;
    }
    *q = Query();
    q->database = json.value(QStringLiteral("database")).toString();
    for(const QJsonValue &v : json.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject s = v.toObject();
        q->sources.append({s.value(QStringLiteral("table")).toString(),
                           s.value(QStringLiteral("alias")).toString()});
    }
    const int n = int(q->sources.size());
    const auto valid = [n](int s) { return s >= 0 && s < n; };
    for(const QJsonValue &v : json.value(QStringLiteral("joins")).toArray()) {
        const QJsonObject o = v.toObject();
        Join j;
        j.left = o.value(QStringLiteral("left")).toInt(-1);
        j.right = o.value(QStringLiteral("right")).toInt(-1);
        j.type = JoinType(indexOf(kJoinTypes, o.value(QStringLiteral("type")).toString()));
        for(const QJsonValue &pair : o.value(QStringLiteral("on")).toArray())
            j.on.append({pair.toArray().at(0).toString(), pair.toArray().at(1).toString()});
        if(valid(j.left) && valid(j.right) && j.left != j.right && !j.on.isEmpty())
            q->joins.append(j);
    }
    for(const QJsonValue &v : json.value(QStringLiteral("columns")).toArray()) {
        const QJsonObject o = v.toObject();
        Column c;
        c.source = o.value(QStringLiteral("source")).toInt(-1);
        c.column = o.value(QStringLiteral("column")).toString();
        c.alias = o.value(QStringLiteral("alias")).toString();
        c.aggregate = o.value(QStringLiteral("aggregate")).toString();
        c.sort = Sort(indexOf(kSorts, o.value(QStringLiteral("sort")).toString()));
        c.criteria = o.value(QStringLiteral("criteria")).toString();
        c.orCriteria = o.value(QStringLiteral("or")).toString();
        c.output = o.value(QStringLiteral("output")).toBool(true);
        if(valid(c.source) && !c.column.isEmpty())
            q->columns.append(c);
    }
    q->distinct = json.value(QStringLiteral("distinct")).toBool();
    q->limit = std::max(0, json.value(QStringLiteral("limit")).toInt());
    return true;
}

} // namespace QueryBuild
