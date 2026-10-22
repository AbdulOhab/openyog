/* OpenYog — the Query Builder's SQL; see QueryBuild.h. */
#include "QueryBuild.h"

#include <QRegularExpression>

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

    QStringList where, having;
    for(const Column &c : q.columns) {
        const QString cond = condition(expression(c), c.criteria);
        if(!cond.isEmpty())
            (c.aggregate.isEmpty() ? where : having) << cond;
    }
    if(!where.isEmpty())
        sql += QStringLiteral("\nWHERE ") + where.join(QStringLiteral("\n    AND "));
    if(!groupBy.isEmpty())
        sql += QStringLiteral("\nGROUP BY ") + groupBy.join(QStringLiteral(", "));
    if(!having.isEmpty())
        sql += QStringLiteral("\nHAVING ") + having.join(QStringLiteral("\n    AND "));

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

} // namespace QueryBuild
