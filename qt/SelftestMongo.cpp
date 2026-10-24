/* OpenYog — --mongotest=host:port:user:password: MongoClient and MongoJson
 * against a live server, in a scratch database of their own
 * (openyog_test_mongo, dropped before and after): every BSON type the shell
 * notation knows goes in and comes back the same, and find, count,
 * aggregate, replace, delete and indexes do what they say. */
#include "MongoJson.h"
#include "Selftest.h"
#include "db/MongoClient.h"

#include <QTextStream>

namespace Selftest {

namespace {

const QString kDb = QStringLiteral("openyog_test_mongo");
const QString kColl = QStringLiteral("people");

QByteArray canonical(const QString &shell)
{
    MongoJson::Value v;
    QString error;
    if(!MongoJson::parse(shell, &v, &error))
        QTextStream(stdout) << "mongotest: bad test input: " << error << '\n';
    return MongoJson::toCanonical(v);
}

QString shell(const QByteArray &canonicalJson)
{
    MongoJson::Value v;
    MongoJson::parse(QString::fromUtf8(canonicalJson), &v);
    return MongoJson::toShell(v, false);
}

} // namespace

int mongoTest(const QString &server)
{
    const QStringList p = server.split(QLatin1Char(':'));
    if(p.size() != 4) {
        QTextStream(stdout) << "mongotest: need host:port:user:password\n";
        return 2;
    }
    ConnectionParams cp;
    cp.driverType = SqlDriverType::Mongo;
    cp.host = p[0];
    cp.port = p[1].toInt();
    cp.user = p[2];
    cp.password = p[3];
    cp.authDatabase = QStringLiteral("admin");

    int passed = 0, failed = 0;
    QString error;
    /* `ok` and `detail` are worked out before the call (argument order is
     * unspecified): no side effects in them */
    const auto check = [&](const QString &what, bool ok, const QString &detail = {}) {
        (ok ? passed : failed)++;
        QTextStream(stdout) << "mongotest " << what << ": " << (ok ? "PASS" : "FAIL")
                            << (detail.isEmpty() ? QString() : QStringLiteral(" (") + detail + ')')
                            << '\n';
        error.clear();
    };

    {
        ConnectionParams wrong = cp;
        wrong.password += QStringLiteral("-wrong");
        const auto none = MongoClient::connect(wrong, &error);
        check(QStringLiteral("a wrong password is refused"),
              !none && error.contains(QStringLiteral("uthentication")), error);
    }
    error.clear();
    const auto m = MongoClient::connect(cp, &error);
    if(!m) {
        QTextStream(stdout) << "mongotest: connect failed: " << error << '\n';
        return 1;
    }
    QTextStream(stdout) << "mongotest server: " << m->serverVersion() << " at " << m->address()
                        << '\n';
    check(QStringLiteral("server version"), !m->serverVersion().isEmpty());

    m->dropDatabase(kDb, nullptr);
    bool ok = m->createCollection(kDb, kColl, &error);
    check(QStringLiteral("create collection"), ok, error);

    const QStringList docs = {
        QStringLiteral("{_id: 1, name: 'Alice', age: 30, visits: NumberLong(5), score: 4.5, "
                       "whole: 2.0, hired: ISODate('2024-01-15T10:30:00.250Z'), "
                       "tags: ['a', 'b'], addr: {city: 'Dhaka', zip: null}}"),
        QStringLiteral("{_id: 2, name: 'রহিম', age: 41, bin: BinData(0, 'AAEC'), "
                       "uid: UUID('0e3f5c1a-2b4d-4e6f-8a9b-0c1d2e3f4a5b'), big: 9007199254740993}"),
        QStringLiteral("{_id: 3, name: 'Dana', age: 25, price: NumberDecimal('9.99'), re: /^a/i, "
                       "ts: Timestamp(1700000000, 3), small: NumberInt(7), lo: MinKey()}"),
        QStringLiteral("{_id: ObjectId(), name: 'Eve', age: 35}"),
    };
    QList<QByteArray> canonicalDocs;
    for(const QString &d : docs)
        canonicalDocs << canonical(d);
    ok = m->insert(kDb, kColl, canonicalDocs, &error);
    check(QStringLiteral("insert"), ok, error);

    ok = m->count(kDb, kColl, "{}", &error) == 4;
    check(QStringLiteral("count all"), ok, error);
    ok = m->count(kDb, kColl, canonical("{age: {$gte: 30}}"), &error) == 3;
    check(QStringLiteral("count with a filter"), ok, error);

    QStringList names;
    MongoClient::Find f;
    f.filter = canonical("{age: {$gte: 30}}");
    f.sort = canonical("{age: -1}");
    f.projection = canonical("{name: 1, _id: 0}");
    m->find(
        kDb, kColl, f,
        [&](const QByteArray &doc) {
            names << shell(doc);
            return true;
        },
        &error);
    check(QStringLiteral("find: filter, sort, projection"),
          names ==
              QStringList({QStringLiteral("{name: \"রহিম\"}"), QStringLiteral("{name: \"Eve\"}"),
                           QStringLiteral("{name: \"Alice\"}")}),
          names.join(QLatin1Char(' ')));

    names.clear();
    f = MongoClient::Find();
    f.sort = canonical("{age: 1}");
    f.skip = 1;
    f.limit = 2;
    f.projection = canonical("{name: 1}");
    m->find(
        kDb, kColl, f,
        [&](const QByteArray &doc) {
            names << shell(doc);
            return true;
        },
        &error);
    /* by age: Dana, Alice, Eve, রহিম; Eve's _id is a new ObjectId */
    check(QStringLiteral("find: skip and limit"),
          names.size() == 2 && names[0] == QStringLiteral("{_id: 1, name: \"Alice\"}") &&
              names[1].startsWith(QStringLiteral("{_id: ObjectId(\"")) &&
              names[1].endsWith(QStringLiteral("name: \"Eve\"}")),
          names.join(QLatin1Char(' ')));

    /* every type, read back in shell notation and written again: the same */
    const auto one = [&](const QString &id) {
        QByteArray out;
        MongoClient::Find byId;
        byId.filter = canonical(QStringLiteral("{_id: %1}").arg(id));
        m->find(
            kDb, kColl, byId,
            [&](const QByteArray &doc) {
                out = doc;
                return false;
            },
            &error);
        return out;
    };
    for(const QString &id : {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}) {
        const QByteArray before = one(id);
        const QString asShell = shell(before);
        qint64 matched = 0;
        const bool replaced = m->replace(kDb, kColl, canonical(QStringLiteral("{_id: %1}").arg(id)),
                                         canonical(asShell), &matched, &error);
        const QByteArray after = one(id);
        check(QStringLiteral("document %1 edited as shell text keeps its types").arg(id),
              replaced && matched == 1 && !before.isEmpty() && before == after,
              before == after ? asShell : QString::fromUtf8(before + " -> " + after));
    }
    check(
        QStringLiteral("shell notation of the types"),
        shell(one(QStringLiteral("1"))) ==
            QStringLiteral("{_id: 1, name: \"Alice\", age: 30, visits: NumberLong(5), score: 4.5, "
                           "whole: 2.0, hired: ISODate(\"2024-01-15T10:30:00.250Z\"), "
                           "tags: [\"a\", \"b\"], addr: {city: \"Dhaka\", zip: null}}"),
        shell(one(QStringLiteral("1"))));
    check(QStringLiteral("an int64 past 2^53 stays exact"),
          shell(one(QStringLiteral("2")))
              .contains(QStringLiteral("big: NumberLong(9007199254740993)")),
          shell(one(QStringLiteral("2"))));

    qint64 matched = 0;
    m->replace(kDb, kColl, canonical("{_id: 2}"), canonical("{name: 'রহিম', age: 42}"), &matched,
               &error);
    const QString replaced = shell(one(QStringLiteral("2")));
    check(QStringLiteral("replace"),
          matched == 1 && replaced == QStringLiteral("{_id: 2, name: \"রহিম\", age: 42}"),
          replaced);

    QString total;
    m->aggregate(
        kDb, kColl, canonical("[{$group: {_id: null, total: {$sum: '$age'}}}]"),
        [&](const QByteArray &doc) {
            total = shell(doc);
            return true;
        },
        &error);
    check(QStringLiteral("aggregate"), total == QStringLiteral("{_id: null, total: 132}"),
          total + error);

    ok = m->createIndex(kDb, kColl, canonical("{name: 1, age: -1}"), {}, true, &error);
    check(QStringLiteral("create index"), ok, error);
    QList<QByteArray> indexes;
    m->indexes(kDb, kColl, &indexes, &error);
    QStringList indexText;
    for(const QByteArray &i : std::as_const(indexes))
        indexText << shell(i);
    check(
        QStringLiteral("indexes"),
        indexes.size() == 2 &&
            indexText.join(QLatin1Char(' ')).contains(QStringLiteral("name: \"name_1_age_-1\"")) &&
            indexText.join(QLatin1Char(' ')).contains(QStringLiteral("unique: true")),
        indexText.join(QStringLiteral(" | ")));
    ok = !m->insert(kDb, kColl, {canonical("{_id: 9, name: 'Eve', age: 35}")}, &error) &&
         error.contains(QStringLiteral("duplicate key"));
    check(QStringLiteral("a unique index refuses a duplicate"), ok, error);
    ok = m->dropIndex(kDb, kColl, QStringLiteral("name_1_age_-1"), &error);
    check(QStringLiteral("drop index"), ok, error);

    qint64 deleted = 0;
    ok = m->remove(kDb, kColl, canonical("{age: {$lt: 30}}"), true, &deleted, &error) &&
         deleted == 1 && m->count(kDb, kColl, "{}", nullptr) == 3;
    check(QStringLiteral("delete many"), ok, QString::number(deleted) + QLatin1Char(' ') + error);

    bool listed = false;
    for(const MongoClient::Collection &c : m->collections(kDb, &error))
        listed = listed || (c.name == kColl && c.type == QStringLiteral("collection"));
    check(QStringLiteral("collections"), listed, error);
    ok = m->databases(&error).contains(kDb);
    check(QStringLiteral("databases"), ok, error);
    QByteArray reply;
    ok = m->command(kDb, canonical("{dbStats: 1}"), &reply, &error);
    const QString stats = shell(reply);
    check(QStringLiteral("command"),
          ok && stats.startsWith(QStringLiteral("{db: \"openyog_test_mongo\", collections: ")) &&
              (stats.endsWith(QStringLiteral("ok: 1}")) ||
               stats.endsWith(QStringLiteral("ok: 1.0}"))),
          stats.left(80) + error);
    ok = !m->find(kDb, kColl, {canonical("{$frob: 1}")}, {}, &error) && !error.isEmpty();
    check(QStringLiteral("a bad filter says why"), ok, error);

    ok = m->dropDatabase(kDb, &error) && !m->databases(nullptr).contains(kDb);
    check(QStringLiteral("drop database"), ok, error);
    QTextStream(stdout) << "mongotest: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace Selftest
