/* OpenYog — Schema Sync's comparison and script; see SchemaSync.h. */
#include "SchemaSync.h"
#include "db/IDbConnection.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <functional>

namespace SchemaSync {

namespace {

QString quoteMySql(const QString &ident)
{
    return QLatin1Char('`') + QString(ident).replace(QLatin1Char('`'), QStringLiteral("``")) +
           QLatin1Char('`');
}

QString quoteDouble(const QString &ident)
{
    return QLatin1Char('"') + QString(ident).replace(QLatin1Char('"'), QStringLiteral("\"\"")) +
           QLatin1Char('"');
}

/* the identifier at `at` (`x`, "x", [x] or a bare word) and where it ends */
QString identAt(const QString &s, int at, int *end = nullptr)
{
    while(at < s.size() && s[at].isSpace())
        ++at;
    if(at >= s.size()) {
        if(end)
            *end = at;
        return {};
    }
    const QChar open = s[at];
    const QChar close = open == QLatin1Char('[') ? QLatin1Char(']') : open;
    if(open == QLatin1Char('`') || open == QLatin1Char('"') || open == QLatin1Char('[')) {
        QString out;
        int i = at + 1;
        for(; i < s.size(); ++i) {
            if(s[i] == close) {
                if(close != QLatin1Char(']') && i + 1 < s.size() && s[i + 1] == close) {
                    out += close; /* a doubled quote is a quote */
                    ++i;
                    continue;
                }
                break;
            }
            out += s[i];
        }
        if(end)
            *end = i + 1;
        return out;
    }
    int i = at;
    while(i < s.size() && !s[i].isSpace() && s[i] != QLatin1Char('(') && s[i] != QLatin1Char(',') &&
          s[i] != QLatin1Char('.'))
        ++i;
    if(end)
        *end = i;
    return s.mid(at, i - at);
}

QString collapse(const QString &s)
{
    return s.simplified();
}

/* integer display widths come and go with the server version (int(11) on
 * 5.7, int on 8.0) without meaning anything */
QString normalizeMySql(const QString &definition)
{
    static const QRegularExpression width(
        QStringLiteral("\\b(tinyint|smallint|mediumint|int|integer|bigint)\\(\\d+\\)"),
        QRegularExpression::CaseInsensitiveOption);
    return collapse(QString(definition).replace(width, QStringLiteral("\\1")));
}

/* the table's own schema, spelled into a definition or not, is the same */
QString normalizePostgres(const QString &definition, const QString &schema)
{
    QString d = definition;
    d.replace(quoteDouble(schema) + QLatin1Char('.'), QString());
    d.replace(QRegularExpression(QStringLiteral("(?<![\\w\"])") +
                                 QRegularExpression::escape(schema) + QStringLiteral("\\.")),
              QString());
    return collapse(d);
}

QString normalized(const QString &definition, const Schema &side)
{
    switch(side.driver) {
        case SqlDriverType::Mysql:
            return normalizeMySql(definition);
        case SqlDriverType::Postgres:
            return normalizePostgres(definition, side.database);
        case SqlDriverType::Sqlite:
            break;
    }
    return collapse(definition);
}

/* MySQL: DEFINER=`user`@`host`, which says who created it, not what it is */
QString withoutDefiner(const QString &create)
{
    static const QRegularExpression definer(
        QStringLiteral(
            "\\bDEFINER\\s*=\\s*(?:`[^`]*`|'[^']*'|[^\\s@]+)@(?:`[^`]*`|'[^']*'|\\S+)\\s*"),
        QRegularExpression::CaseInsensitiveOption);
    return QString(create).remove(definer);
}

/* a view's, routine's or trigger's CREATE without what differs between two
 * copies of the same one: MySQL's definer, the database / schema's own name
 * qualifying what is in it, a trailing ';', the layout */
QString objectNormalized(const QString &create, const Schema &side)
{
    QString s = create.trimmed();
    while(s.endsWith(QLatin1Char(';')))
        s.chop(1);
    switch(side.driver) {
        case SqlDriverType::Mysql:
            return collapse(withoutDefiner(s).remove(quoteMySql(side.database) + QLatin1Char('.')));
        case SqlDriverType::Postgres:
            return normalizePostgres(s, side.database);
        case SqlDriverType::Sqlite:
            break;
    }
    return collapse(s);
}

/* PostgreSQL: the source schema's name, wherever it qualifies something,
 * becomes the target's */
QString moveSchema(const QString &sql, const QString &from, const QString &to)
{
    if(from == to)
        return sql;
    QString s = sql;
    s.replace(quoteDouble(from) + QLatin1Char('.'), quoteDouble(to) + QLatin1Char('.'));
    s.replace(QRegularExpression(QStringLiteral("(?<![\\w\"])") + QRegularExpression::escape(from) +
                                 QStringLiteral("\\.")),
              quoteDouble(to) + QLatin1Char('.'));
    return s;
}

/* the top-level comma-separated items of a CREATE TABLE's column list */
QStringList splitTopLevel(const QString &body)
{
    QStringList items;
    QString cur;
    int depth = 0;
    QChar quote;
    for(int i = 0; i < body.size(); ++i) {
        const QChar c = body[i];
        if(!quote.isNull()) {
            cur += c;
            if(c == quote)
                quote = QChar();
            continue;
        }
        if(c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')) {
            quote = c;
        } else if(c == QLatin1Char('[')) {
            quote = QLatin1Char(']');
        } else if(c == QLatin1Char('(')) {
            ++depth;
        } else if(c == QLatin1Char(')')) {
            --depth;
        } else if(c == QLatin1Char(',') && depth == 0) {
            items << cur.trimmed();
            cur.clear();
            continue;
        }
        cur += c;
    }
    if(!cur.trimmed().isEmpty())
        items << cur.trimmed();
    return items;
}

const Part *findPart(const QList<Part> &parts, const QString &name)
{
    for(const Part &p : parts)
        if(p.name == name)
            return &p;
    return nullptr;
}

} // namespace

Table parseMySql(const QString &name, const QString &showCreate)
{
    static const QRegularExpression autoInc(QStringLiteral("\\s*AUTO_INCREMENT=\\d+"));
    Table t;
    t.name = name;
    t.create = QString(showCreate).replace(autoInc, QString());
    const QStringList lines = showCreate.split(QLatin1Char('\n'));
    for(int i = 1; i < lines.size(); ++i) {
        QString l = lines[i].trimmed();
        if(l.endsWith(QLatin1Char(',')))
            l.chop(1);
        if(l.isEmpty())
            continue;
        if(l.startsWith(QLatin1Char(')'))) {
            t.options = collapse(l.mid(1).replace(autoInc, QString()));
        } else if(l.startsWith(QLatin1Char('`'))) {
            t.columns << Part{identAt(l, 0), l};
        } else if(l.startsWith(QStringLiteral("PRIMARY KEY"))) {
            t.keys << Part{QStringLiteral("PRIMARY"), l};
        } else if(l.startsWith(QStringLiteral("CONSTRAINT"))) {
            int end = 0;
            const QString n = identAt(l, int(QStringLiteral("CONSTRAINT").size()), &end);
            if(l.contains(QStringLiteral(" FOREIGN KEY ")))
                t.foreignKeys << Part{n, l};
            else
                t.keys << Part{n, l}; /* CHECK */
        } else if(l.contains(QStringLiteral("KEY `"))) {
            /* KEY / UNIQUE KEY / FULLTEXT KEY / SPATIAL KEY `name` (…) */
            t.keys << Part{identAt(l, int(l.indexOf(QLatin1Char('`')))), l};
        }
    }
    return t;
}

Table parsePostgres(const QString &name, const QString &create, const QString &foreignKeys)
{
    Table t;
    t.name = name;
    t.create = create;
    for(const QString &statement : create.split(QStringLiteral(";\n"), Qt::SkipEmptyParts)) {
        const QString st = statement.trimmed();
        if(st.startsWith(QStringLiteral("CREATE TABLE"))) {
            const int open = int(st.indexOf(QLatin1Char('('))),
                      close = int(st.lastIndexOf(QLatin1Char(')')));
            for(const QString &item : splitTopLevel(st.mid(open + 1, close - open - 1))) {
                if(item.startsWith(QStringLiteral("PRIMARY KEY")))
                    t.keys << Part{QStringLiteral("PRIMARY"), item};
                else if(item.startsWith(QStringLiteral("CONSTRAINT ")))
                    t.keys << Part{identAt(item, int(QStringLiteral("CONSTRAINT ").size())), item};
                else
                    t.columns << Part{identAt(item, 0), item};
            }
        } else if(st.startsWith(QStringLiteral("CREATE INDEX")) ||
                  st.startsWith(QStringLiteral("CREATE UNIQUE INDEX"))) {
            const int at = int(st.indexOf(QStringLiteral("INDEX "))) + 6;
            t.keys << Part{identAt(st, at), st};
        }
    }
    /* ALTER TABLE s.t ADD CONSTRAINT "name" FOREIGN KEY … */
    static const QString kAdd = QStringLiteral(" ADD CONSTRAINT ");
    for(const QString &statement : foreignKeys.split(QStringLiteral(";\n"), Qt::SkipEmptyParts)) {
        const int at = int(statement.indexOf(kAdd));
        if(at < 0)
            continue;
        int end = 0;
        const QString n = identAt(statement, at + int(kAdd.size()), &end);
        t.foreignKeys << Part{n, statement.mid(end).trimmed()};
    }
    return t;
}

Table parseSqlite(const QString &name, const QString &createTable, const QStringList &createIndexes)
{
    Table t;
    t.name = name;
    QStringList create{createTable};
    const int open = int(createTable.indexOf(QLatin1Char('('))),
              close = int(createTable.lastIndexOf(QLatin1Char(')')));
    int unnamed = 0;
    if(open >= 0 && close > open) {
        for(const QString &item : splitTopLevel(createTable.mid(open + 1, close - open - 1))) {
            const QString first = identAt(item, 0).toUpper();
            if(first == QStringLiteral("CONSTRAINT")) {
                int end = 0;
                identAt(item, 0, &end);
                const QString n = identAt(item, end);
                (item.contains(QStringLiteral("FOREIGN KEY"), Qt::CaseInsensitive) ? t.foreignKeys
                                                                                   : t.keys)
                    << Part{n, item};
            } else if(first == QStringLiteral("FOREIGN")) {
                t.foreignKeys << Part{QStringLiteral("(foreign key %1)").arg(++unnamed), item};
            } else if(first == QStringLiteral("PRIMARY")) {
                t.keys << Part{QStringLiteral("PRIMARY"), item};
            } else if(first == QStringLiteral("UNIQUE") || first == QStringLiteral("CHECK")) {
                t.keys << Part{QStringLiteral("(%1 %2)").arg(first.toLower()).arg(++unnamed), item};
            } else {
                t.columns << Part{identAt(item, 0), item};
            }
        }
    }
    for(const QString &index : createIndexes) {
        static const QRegularExpression nameRe(
            QStringLiteral("INDEX\\s+(?:IF\\s+NOT\\s+EXISTS\\s+)?"),
            QRegularExpression::CaseInsensitiveOption);
        const auto m = nameRe.match(index);
        if(!m.hasMatch())
            continue;
        QString n = identAt(index, int(m.capturedEnd()));
        int end = 0;
        identAt(index, int(m.capturedEnd()), &end);
        if(end < index.size() && index[end] == QLatin1Char('.')) /* schema.index */
            n = identAt(index, end + 1);
        t.keys << Part{n, index.trimmed()};
        create << index.trimmed();
    }
    t.create = create.join(QStringLiteral(";\n"));
    return t;
}

Schema load(IDbConnection *conn, const QString &db)
{
    Schema s;
    s.driver = conn->driverType();
    s.database = db;
    for(const QString &name : conn->listTables(db, QStringLiteral("BASE TABLE"))) {
        QString error;
        const QString create = conn->showCreate(QStringLiteral("TABLE"), db, name, &error);
        switch(s.driver) {
            case SqlDriverType::Mysql:
                s.tables << parseMySql(name, create);
                break;
            case SqlDriverType::Postgres:
                s.tables << parsePostgres(
                    name, create,
                    conn->showCreate(QStringLiteral("FOREIGN KEYS"), db, name, &error));
                break;
            case SqlDriverType::Sqlite: {
                DbResultSet rs;
                conn->query(
                    QStringLiteral("SELECT sql FROM %1.sqlite_master WHERE type = 'index' "
                                   "AND tbl_name = '%2' AND sql IS NOT NULL ORDER BY name")
                        .arg(conn->quoteIdent(db.isEmpty() ? QStringLiteral("main") : db),
                             QString(name).replace(QLatin1Char('\''), QStringLiteral("''"))),
                    &rs, nullptr);
                QStringList indexes;
                for(const QStringList &row : rs.rows)
                    indexes << row.value(0);
                s.tables << parseSqlite(name, create, indexes);
                break;
            }
        }
    }

    /* views, routines and triggers; on PostgreSQL not the ones an extension
     * put there (pg_stat_statements' views, uuid-ossp's functions…): those
     * come and go with CREATE EXTENSION */
    QSet<QString> fromExtensions;
    if(s.driver == SqlDriverType::Postgres) {
        const QString schema = QString(db).replace(QLatin1Char('\''), QStringLiteral("''"));
        DbResultSet rs;
        conn->query(QStringLiteral("SELECT c.relname FROM pg_depend d "
                                   "JOIN pg_class c ON c.oid = d.objid "
                                   "JOIN pg_namespace n ON n.oid = c.relnamespace "
                                   "WHERE d.deptype = 'e' AND d.classid = 'pg_class'::regclass "
                                   "AND n.nspname = '%1' "
                                   "UNION SELECT p.proname FROM pg_depend d "
                                   "JOIN pg_proc p ON p.oid = d.objid "
                                   "JOIN pg_namespace n ON n.oid = p.pronamespace "
                                   "WHERE d.deptype = 'e' AND d.classid = 'pg_proc'::regclass "
                                   "AND n.nspname = '%1'")
                        .arg(schema),
                    &rs, nullptr);
        for(const QStringList &row : rs.rows)
            fromExtensions.insert(row.value(0));
    }
    const auto object = [&](const QString &kind, const QString &name) {
        QString error;
        return Object{kind, name, conn->showCreate(kind, db, name, &error).trimmed()};
    };
    QList<Object> routines, views, triggers;
    QSet<QString> seen;
    for(const QStringList &row : conn->listRoutines(db).rows) {
        const QString kind = row.value(1).toUpper() == QStringLiteral("PROCEDURE")
                                 ? QStringLiteral("PROCEDURE")
                                 : QStringLiteral("FUNCTION");
        /* an overloaded PostgreSQL routine: showCreate() reads one of them */
        if(fromExtensions.contains(row.value(0)) || seen.contains(kind + row.value(0)))
            continue;
        seen.insert(kind + row.value(0));
        routines << object(kind, row.value(0));
    }
    for(const QString &name : conn->listTables(db, QStringLiteral("VIEW")))
        if(!fromExtensions.contains(name))
            views << object(QStringLiteral("VIEW"), name);
    for(const QString &name : conn->listTriggers(db))
        triggers << object(QStringLiteral("TRIGGER"), name);
    const auto byName = [](const Object &a, const Object &b) {
        return a.kind == b.kind ? a.name < b.name : a.kind < b.kind;
    };
    std::sort(routines.begin(), routines.end(), byName);
    std::sort(views.begin(), views.end(), byName);
    std::sort(triggers.begin(), triggers.end(), byName);
    for(const QList<Object> &list : {routines, orderViews(views), triggers})
        for(const Object &o : list)
            if(!o.create.isEmpty())
                s.objects << o;
    return s;
}

QList<Object> orderViews(const QList<Object> &views)
{
    /* B before A when A's CREATE names B: as a word, quoted or not */
    const auto names = [](const Object &a, const Object &b) {
        const QRegularExpression word(QStringLiteral("(?<![\\w$])") +
                                      QRegularExpression::escape(b.name) +
                                      QStringLiteral("(?![\\w$])"));
        return a.create.contains(word);
    };
    QList<Object> ordered;
    QSet<QString> done, onPath;
    const std::function<void(int)> visit = [&](int i) {
        const Object &v = views[i];
        if(done.contains(v.name) || onPath.contains(v.name))
            return; /* a cycle is cut where the walk meets itself */
        onPath.insert(v.name);
        for(int j = 0; j < views.size(); ++j)
            if(j != i && names(v, views[j]))
                visit(j);
        onPath.remove(v.name);
        done.insert(v.name);
        ordered << v;
    };
    for(int i = 0; i < views.size(); ++i)
        visit(i);
    return ordered;
}

namespace {

/* the statements one change takes, per dialect */
class Writer
{
public:
    Writer(const Schema &source, const Schema &target) : m_source(source), m_target(target) {}

    QString table(const QString &name) const
    {
        switch(m_target.driver) {
            case SqlDriverType::Mysql:
                return quoteMySql(name); /* the script starts with USE target */
            case SqlDriverType::Postgres:
                return quoteDouble(m_target.database) + QLatin1Char('.') + quoteDouble(name);
            case SqlDriverType::Sqlite:
                break;
        }
        return quoteDouble(name);
    }
    QString ident(const QString &name) const
    {
        return m_target.driver == SqlDriverType::Mysql ? quoteMySql(name) : quoteDouble(name);
    }
    QString alter(const QString &tableName, const QString &clause) const
    {
        return QStringLiteral("ALTER TABLE %1 %2").arg(table(tableName), clause);
    }
    QString fromSource(const QString &sql) const
    {
        return m_target.driver == SqlDriverType::Postgres
                   ? moveSchema(sql, m_source.database, m_target.database)
                   : sql;
    }

    void createTable(Change &c, const Table &t) const
    {
        for(const QString &st : t.create.split(QStringLiteral(";\n"), Qt::SkipEmptyParts))
            c.sql << fromSource(st.trimmed());
    }
    void dropTable(Change &c) const
    {
        c.sql << QStringLiteral("DROP TABLE %1").arg(table(c.table));
    }

    void addColumn(Change &c, const QString &previous) const
    {
        const QString clause = QStringLiteral("ADD COLUMN ") + fromSource(c.source);
        if(m_target.driver == SqlDriverType::Mysql)
            c.sql << alter(c.table, clause + (previous.isEmpty()
                                                  ? QStringLiteral(" FIRST")
                                                  : QStringLiteral(" AFTER ") + ident(previous)));
        else
            c.sql << alter(c.table, clause);
    }
    void dropColumn(Change &c) const
    {
        c.sql << alter(c.table, QStringLiteral("DROP COLUMN ") + ident(c.name));
    }
    void alterColumn(Change &c) const
    {
        switch(m_target.driver) {
            case SqlDriverType::Mysql:
                c.sql << alter(c.table, QStringLiteral("MODIFY COLUMN ") + c.source);
                return;
            case SqlDriverType::Sqlite:
                c.note =
                    QStringLiteral("SQLite can't change a column in place: the table has to be "
                                   "rebuilt (new table, copy the rows, drop, rename)");
                return;
            case SqlDriverType::Postgres:
                break;
        }
        /* "name" type [NOT NULL] [DEFAULT expr], one ALTER COLUMN per part */
        struct Parts
        {
            QString type, def;
            bool notNull = false;
        };
        const auto parts = [&](const QString &line, const QString &schema) {
            int end = 0;
            identAt(line, 0, &end);
            QString rest = line.mid(end).trimmed();
            Parts p;
            const int defAt = int(rest.indexOf(QStringLiteral(" DEFAULT ")));
            if(defAt >= 0) {
                p.def = rest.mid(defAt + 9).trimmed();
                rest = rest.left(defAt);
            }
            if(rest.endsWith(QStringLiteral(" NOT NULL"))) {
                p.notNull = true;
                rest.chop(9);
            }
            p.type = rest.trimmed();
            p.def = normalizePostgres(p.def, schema);
            return p;
        };
        const Parts from = parts(c.target, m_target.database),
                    to = parts(c.source, m_source.database);
        const QString col = ident(c.name);
        if(to.type.endsWith(QStringLiteral("serial")) && to.type != from.type) {
            c.note = QStringLiteral("a column can't become %1 in place: create its sequence and "
                                    "set the default by hand")
                         .arg(to.type);
            return;
        }
        if(to.type != from.type)
            c.sql << alter(
                c.table, QStringLiteral("ALTER COLUMN %1 TYPE %2 USING %1::%2").arg(col, to.type));
        if(to.notNull != from.notNull)
            c.sql << alter(c.table, QStringLiteral("ALTER COLUMN %1 %2 NOT NULL")
                                        .arg(col, to.notNull ? QStringLiteral("SET")
                                                             : QStringLiteral("DROP")));
        if(to.def != from.def)
            c.sql << alter(c.table, to.def.isEmpty()
                                        ? QStringLiteral("ALTER COLUMN %1 DROP DEFAULT").arg(col)
                                        : QStringLiteral("ALTER COLUMN %1 SET DEFAULT %2")
                                              .arg(col, moveSchema(to.def, m_source.database,
                                                                   m_target.database)));
        if(c.sql.isEmpty())
            c.note =
                QStringLiteral("the two definitions differ in a way that needs a look by hand");
    }

    QStringList addKey(const QString &tableName, const Part &key) const
    {
        const QString &def = key.definition;
        if(def.startsWith(QStringLiteral("CREATE ")))
            return {fromSource(def)}; /* a PostgreSQL / SQLite index of its own */
        if(m_target.driver == SqlDriverType::Sqlite)
            return {};
        return {alter(tableName, QStringLiteral("ADD ") + def)};
    }
    QStringList dropKey(const QString &tableName, const Part &key) const
    {
        const QString &def = key.definition;
        if(def.startsWith(QStringLiteral("CREATE "))) {
            const QString index =
                m_target.driver == SqlDriverType::Postgres
                    ? quoteDouble(m_target.database) + QLatin1Char('.') + quoteDouble(key.name)
                    : quoteDouble(key.name);
            return {QStringLiteral("DROP INDEX %1").arg(index)};
        }
        switch(m_target.driver) {
            case SqlDriverType::Mysql:
                if(key.name == QStringLiteral("PRIMARY"))
                    return {alter(tableName, QStringLiteral("DROP PRIMARY KEY"))};
                if(def.startsWith(QStringLiteral("CONSTRAINT")))
                    return {alter(tableName, QStringLiteral("DROP CONSTRAINT ") + ident(key.name))};
                return {alter(tableName, QStringLiteral("DROP INDEX ") + ident(key.name))};
            case SqlDriverType::Postgres:
                /* the primary key's constraint keeps PostgreSQL's default name */
                return {alter(tableName, QStringLiteral("DROP CONSTRAINT ") +
                                             ident(key.name == QStringLiteral("PRIMARY")
                                                       ? tableName + QStringLiteral("_pkey")
                                                       : key.name))};
            case SqlDriverType::Sqlite:
                break;
        }
        return {};
    }
    void tableConstraintNote(Change &c) const
    {
        c.note = QStringLiteral("SQLite can't change a table's constraints in place: the table "
                                "has to be rebuilt");
    }

    QString addForeignKey(const QString &tableName, const Part &fk) const
    {
        if(m_target.driver == SqlDriverType::Mysql)
            return alter(tableName, QStringLiteral("ADD ") + fk.definition);
        /* qualify the referenced table with the target schema: unqualified it
         * would resolve through the search path, to the source's */
        static const QRegularExpression refs(
            QStringLiteral("REFERENCES\\s+(\"(?:[^\"]|\"\")+\"|[A-Za-z_][\\w$]*)\\s*\\("));
        QString def = normalizePostgres(fk.definition, m_source.database);
        def.replace(refs, QStringLiteral("REFERENCES ") + quoteDouble(m_target.database) +
                              QStringLiteral(".\\1("));
        return alter(tableName, QStringLiteral("ADD CONSTRAINT %1 %2").arg(ident(fk.name), def));
    }
    QString dropForeignKey(const QString &tableName, const QString &name) const
    {
        return m_target.driver == SqlDriverType::Mysql
                   ? alter(tableName, QStringLiteral("DROP FOREIGN KEY ") + ident(name))
                   : alter(tableName, QStringLiteral("DROP CONSTRAINT ") + ident(name));
    }

    /* the source's CREATE, for the target */
    QString objectSql(const Object &o) const
    {
        QString s = o.create.trimmed();
        while(s.endsWith(QLatin1Char(';')))
            s.chop(1);
        switch(m_target.driver) {
            case SqlDriverType::Mysql:
                /* unqualified, it lands in the target: the script USEs it */
                return withoutDefiner(s).remove(quoteMySql(m_source.database) + QLatin1Char('.'));
            case SqlDriverType::Postgres:
                return moveSchema(s, m_source.database, m_target.database);
            case SqlDriverType::Sqlite:
                break;
        }
        return s;
    }
    static QString orReplace(const QString &create)
    {
        static const QRegularExpression lead(
            QStringLiteral("^\\s*CREATE\\s+(?:OR\\s+REPLACE\\s+)?"),
            QRegularExpression::CaseInsensitiveOption);
        return QString(create).replace(lead, QStringLiteral("CREATE OR REPLACE "));
    }
    /* `o` is the target's */
    QString dropObject(const Object &o, QString *note) const
    {
        switch(m_target.driver) {
            case SqlDriverType::Mysql:
                return QStringLiteral("DROP %1 IF EXISTS %2").arg(o.kind, quoteMySql(o.name));
            case SqlDriverType::Postgres: {
                if(o.kind != QStringLiteral("TRIGGER"))
                    return QStringLiteral("DROP %1 IF EXISTS %2.%3")
                        .arg(o.kind, quoteDouble(m_target.database), quoteDouble(o.name));
                /* DROP TRIGGER name ON table: the table from its definition */
                static const QRegularExpression on(
                    QStringLiteral("\\bON\\s+((?:(?:\"(?:[^\"]|\"\")+\"|[A-Za-z_][\\w$]*)\\.)?"
                                   "(?:\"(?:[^\"]|\"\")+\"|[A-Za-z_][\\w$]*))\\s+FOR\\b"),
                    QRegularExpression::CaseInsensitiveOption);
                const auto m = on.match(o.create);
                if(!m.hasMatch()) {
                    *note = QStringLiteral("the trigger's table isn't in its definition");
                    return {};
                }
                return QStringLiteral("DROP TRIGGER IF EXISTS %1 ON %2")
                    .arg(quoteDouble(o.name), m.captured(1));
            }
            case SqlDriverType::Sqlite:
                break;
        }
        return QStringLiteral("DROP %1 IF EXISTS %2").arg(o.kind, quoteDouble(o.name));
    }
    /* a body of statements of its own, which a plain ';' would cut short;
     * PostgreSQL's are dollar-quoted already */
    bool compound(const QString &kind) const
    {
        switch(m_target.driver) {
            case SqlDriverType::Mysql:
                return kind != QStringLiteral("VIEW");
            case SqlDriverType::Sqlite:
                return kind == QStringLiteral("TRIGGER");
            case SqlDriverType::Postgres:
                break;
        }
        return false;
    }
    void createObject(Change &c, const Object &o) const
    {
        c.sql << objectSql(o);
        c.compound = compound(o.kind);
    }
    /* replaced in place where the dialect can (what depends on it stays),
     * dropped and created again where it can't */
    void alterObject(Change &c, const Object &from, const Object &to) const
    {
        const bool postgres = m_target.driver == SqlDriverType::Postgres;
        c.compound = compound(from.kind);
        if(from.kind == QStringLiteral("VIEW") && m_target.driver != SqlDriverType::Sqlite) {
            c.sql << orReplace(objectSql(from));
        } else if(postgres && from.kind != QStringLiteral("TRIGGER")) {
            c.sql << orReplace(objectSql(from)); /* pg_get_functiondef's own form */
        } else {
            const QString drop = dropObject(to, &c.note);
            if(drop.isEmpty())
                return;
            c.prepare << drop;
            c.sql << objectSql(from);
        }
    }

private:
    const Schema &m_source;
    const Schema &m_target;
};

const Table *findTable(const Schema &s, const QString &name)
{
    for(const Table &t : s.tables)
        if(t.name == name)
            return &t;
    return nullptr;
}

const Object *findObject(const Schema &s, const QString &kind, const QString &name)
{
    for(const Object &o : s.objects)
        if(o.kind == kind && o.name == name)
            return &o;
    return nullptr;
}

} // namespace

QList<Change> compare(const Schema &source, const Schema &target)
{
    const Writer w(source, target);
    const bool sqlite = target.driver == SqlDriverType::Sqlite;
    const auto same = [&](const QString &a, const QString &b) {
        return normalized(a, source) == normalized(b, target);
    };
    QList<Change> out;
    for(const Table &s : source.tables) {
        const Table *t = findTable(target, s.name);
        if(!t) {
            Change c;
            c.action = Action::CreateTable;
            c.table = s.name;
            c.source = s.create;
            w.createTable(c, s);
            out << c;
            continue;
        }

        for(int i = 0; i < s.columns.size(); ++i) {
            const Part &col = s.columns[i];
            const Part *tc = findPart(t->columns, col.name);
            Change c;
            c.table = s.name;
            c.name = col.name;
            c.source = col.definition;
            if(!tc) {
                c.action = Action::AddColumn;
                w.addColumn(c, i == 0 ? QString() : s.columns[i - 1].name);
            } else if(!same(col.definition, tc->definition)) {
                c.action = Action::AlterColumn;
                c.target = tc->definition;
                w.alterColumn(c);
            } else {
                continue;
            }
            out << c;
        }
        for(const Part &tc : t->columns) {
            if(findPart(s.columns, tc.name))
                continue;
            Change c;
            c.action = Action::DropColumn;
            c.table = s.name;
            c.name = tc.name;
            c.target = tc.definition;
            c.pickedByDefault = false;
            w.dropColumn(c);
            out << c;
        }

        /* keys: a SQLite index can come and go; its table constraints can't */
        const auto keyChange = [&](Action action, const Part *src, const Part *tgt) {
            Change c;
            c.action = action;
            c.table = s.name;
            c.name = (src ? src : tgt)->name;
            c.source = src ? src->definition : QString();
            c.target = tgt ? tgt->definition : QString();
            const bool inPlace =
                !sqlite || (src ? src : tgt)->definition.startsWith(QStringLiteral("CREATE "));
            if(!inPlace) {
                w.tableConstraintNote(c);
            } else if(action == Action::AddKey) {
                c.sql = w.addKey(s.name, *src);
            } else if(action == Action::DropKey) {
                c.sql = w.dropKey(s.name, *tgt);
            } else {
                c.prepare = w.dropKey(s.name, *tgt);
                c.sql = w.addKey(s.name, *src);
            }
            out << c;
        };
        for(const Part &k : s.keys) {
            const Part *tk = findPart(t->keys, k.name);
            if(!tk)
                keyChange(Action::AddKey, &k, nullptr);
            else if(!same(k.definition, tk->definition))
                keyChange(Action::AlterKey, &k, tk);
        }
        for(const Part &tk : t->keys)
            if(!findPart(s.keys, tk.name))
                keyChange(Action::DropKey, nullptr, &tk);

        const auto fkChange = [&](Action action, const Part *src, const Part *tgt) {
            Change c;
            c.action = action;
            c.table = s.name;
            c.name = (src ? src : tgt)->name;
            c.source = src ? src->definition : QString();
            c.target = tgt ? tgt->definition : QString();
            if(sqlite) {
                c.note = QStringLiteral("SQLite can't add or drop a foreign key in place: the "
                                        "table has to be rebuilt");
            } else {
                if(tgt)
                    (action == Action::DropForeignKey ? c.sql : c.prepare)
                        << w.dropForeignKey(s.name, tgt->name);
                if(src)
                    c.sql << w.addForeignKey(s.name, *src);
            }
            out << c;
        };
        for(const Part &f : s.foreignKeys) {
            const Part *tf = findPart(t->foreignKeys, f.name);
            if(!tf)
                fkChange(Action::AddForeignKey, &f, nullptr);
            else if(!same(f.definition, tf->definition))
                fkChange(Action::AlterForeignKey, &f, tf);
        }
        for(const Part &tf : t->foreignKeys)
            if(!findPart(s.foreignKeys, tf.name))
                fkChange(Action::DropForeignKey, nullptr, &tf);

        if(source.driver == SqlDriverType::Mysql && !same(s.options, t->options)) {
            Change c;
            c.action = Action::AlterOptions;
            c.table = s.name;
            c.source = s.options;
            c.target = t->options;
            c.pickedByDefault = false;
            c.sql << w.alter(s.name, s.options);
            out << c;
        }
    }
    for(const Table &t : target.tables) {
        if(findTable(source, t.name))
            continue;
        Change c;
        c.action = Action::DropTable;
        c.table = t.name;
        c.target = t.create;
        c.pickedByDefault = false;
        w.dropTable(c);
        out << c;
    }

    for(const Object &so : source.objects) {
        const Object *to = findObject(target, so.kind, so.name);
        if(to && objectNormalized(so.create, source) == objectNormalized(to->create, target))
            continue;
        Change c;
        c.kind = so.kind;
        c.table = so.name;
        c.source = so.create;
        if(to) {
            c.action = Action::AlterObject;
            c.target = to->create;
            w.alterObject(c, so, *to);
        } else {
            c.action = Action::CreateObject;
            w.createObject(c, so);
        }
        out << c;
    }
    for(const Object &to : target.objects) {
        if(findObject(source, to.kind, to.name))
            continue;
        Change c;
        c.action = Action::DropObject;
        c.kind = to.kind;
        c.table = to.name;
        c.target = to.create;
        c.pickedByDefault = false;
        const QString drop = w.dropObject(to, &c.note);
        if(!drop.isEmpty())
            c.sql << drop;
        out << c;
    }
    return out;
}

QString describe(const Change &c)
{
    const QString n = c.name;
    switch(c.action) {
        case Action::CreateTable:
            return QStringLiteral("create table");
        case Action::DropTable:
            return QStringLiteral("drop table");
        case Action::AddColumn:
            return QStringLiteral("add column %1").arg(n);
        case Action::AlterColumn:
            return QStringLiteral("change column %1").arg(n);
        case Action::DropColumn:
            return QStringLiteral("drop column %1").arg(n);
        case Action::AddKey:
            return QStringLiteral("add key %1").arg(n);
        case Action::AlterKey:
            return QStringLiteral("change key %1").arg(n);
        case Action::DropKey:
            return QStringLiteral("drop key %1").arg(n);
        case Action::AddForeignKey:
            return QStringLiteral("add foreign key %1").arg(n);
        case Action::AlterForeignKey:
            return QStringLiteral("change foreign key %1").arg(n);
        case Action::DropForeignKey:
            return QStringLiteral("drop foreign key %1").arg(n);
        case Action::AlterOptions:
            return QStringLiteral("change table options");
        case Action::CreateObject:
            return QStringLiteral("create %1").arg(c.kind.toLower());
        case Action::AlterObject:
            return QStringLiteral("change %1").arg(c.kind.toLower());
        case Action::DropObject:
            return QStringLiteral("drop %1").arg(c.kind.toLower());
    }
    return {};
}

QString script(const QList<Change> &changes, const Schema &source, const Schema &target)
{
    /* the order statements can run in: views, routines and triggers out
     * first (one may read what the rest takes away), then foreign keys (an
     * index or column they use can't go while they're there), columns and
     * keys, the missing tables, the extra ones, foreign keys back in
     * (everything they point at exists by then), and last the routines, the
     * views (they may call the routines) and the triggers */
    const auto phase = [](const Change &c) {
        switch(c.action) {
            case Action::DropObject:
                return 0;
            case Action::DropForeignKey:
                return 1;
            case Action::DropKey:
                return 2;
            case Action::DropColumn:
                return 3;
            case Action::AlterColumn:
                return 4;
            case Action::AddColumn:
                return 5;
            case Action::AddKey:
            case Action::AlterKey:
                return 6;
            case Action::AlterOptions:
                return 7;
            case Action::CreateTable:
                return 8;
            case Action::DropTable:
                return 9;
            case Action::AddForeignKey:
            case Action::AlterForeignKey:
                return 10;
            case Action::CreateObject:
            case Action::AlterObject:
                return c.kind == QStringLiteral("VIEW")      ? 12
                       : c.kind == QStringLiteral("TRIGGER") ? 13
                                                             : 11;
        }
        return 13;
    };
    constexpr int kPhases = 14;
    QList<QStringList> blocks(kPhases);
    QStringList notDone;
    bool objects = false;
    for(const Change &c : changes) {
        const QString title = QStringLiteral("-- %1: %2").arg(c.table, describe(c));
        if(c.sql.isEmpty() && c.prepare.isEmpty()) {
            notDone << QStringLiteral("--   %1: %2 (%3)").arg(c.table, describe(c), c.note);
            continue;
        }
        objects = objects || c.action == Action::CreateObject || c.action == Action::AlterObject;
        if(!c.prepare.isEmpty()) {
            /* an altered object / key / foreign key: its DROP with the drops */
            const int at = c.action == Action::AlterObject       ? 0
                           : c.action == Action::AlterForeignKey ? 1
                                                                 : 2;
            blocks[at] << title + QStringLiteral(" (drop the old one)");
            for(const QString &st : c.prepare)
                blocks[at] << st + QLatin1Char(';');
        }
        QStringList &b = blocks[phase(c)];
        b << title;
        if(c.compound) {
            /* the newline before $$: a body may end in a '$' of its own */
            b << QStringLiteral("DELIMITER $$");
            for(const QString &st : c.sql)
                b << st + QStringLiteral("\n$$");
            b << QStringLiteral("DELIMITER ;");
        } else {
            for(const QString &st : c.sql)
                b << st + QLatin1Char(';');
        }
    }

    QStringList out;
    out << QStringLiteral("-- Schema sync: make %1 match %2").arg(target.database, source.database);
    switch(target.driver) {
        case SqlDriverType::Mysql:
            out << QStringLiteral("USE %1;").arg(quoteMySql(target.database))
                << QStringLiteral("SET FOREIGN_KEY_CHECKS = 0;");
            break;
        case SqlDriverType::Postgres:
        case SqlDriverType::Sqlite:
            out << QStringLiteral("BEGIN;");
            break;
    }
    /* a view's or trigger's CREATE names what is in its own schema without
     * it: found in the target's schema first while the script runs */
    if(objects && target.driver == SqlDriverType::Postgres && source.database != target.database)
        out << QStringLiteral("SELECT set_config('search_path', '%1, ' || "
                              "current_setting('search_path'), true);")
                   .arg(quoteDouble(target.database)
                            .replace(QLatin1Char('\''), QStringLiteral("''")));
    for(const QStringList &b : std::as_const(blocks))
        if(!b.isEmpty())
            out << QString() << b;
    out << QString();
    out << (target.driver == SqlDriverType::Mysql ? QStringLiteral("SET FOREIGN_KEY_CHECKS = 1;")
                                                  : QStringLiteral("COMMIT;"));
    if(!notDone.isEmpty())
        out << QString() << QStringLiteral("-- Not done here:") << notDone;
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace SchemaSync
