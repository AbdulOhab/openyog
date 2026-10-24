/* OpenYog — MongoDB through libmongoc; see MongoClient.h. */
#include "MongoClient.h"

#ifdef OPENYOG_HAVE_MONGO

#include "SshTunnel.h"

#include <mongoc/mongoc.h>

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace {

void startDriver()
{
    static std::once_flag once;
    std::call_once(once, [] {
        mongoc_init();
        /* no log lines on stderr: every failure comes back as an error */
        mongoc_log_set_handler(nullptr, nullptr);
        /* libmongoc's globals go when the program does (after the tabs
         * holding clients are gone) */
        static struct Cleanup
        {
            ~Cleanup()
            {
                mongoc_cleanup();
            }
        } cleanup;
    });
}

QString message(const bson_error_t &e)
{
    return QString::fromUtf8(e.message);
}

/* owns a bson_t (not a unique_ptr: bson_t's alignment attribute doesn't
 * survive being a template argument) */
class Bson
{
public:
    explicit Bson(bson_t *b = nullptr) : m_b(b) {}
    Bson(Bson &&o) noexcept : m_b(std::exchange(o.m_b, nullptr)) {}
    Bson &operator=(Bson &&o) noexcept
    {
        std::swap(m_b, o.m_b);
        return *this;
    }
    Bson(const Bson &) = delete;
    Bson &operator=(const Bson &) = delete;
    ~Bson()
    {
        if(m_b)
            bson_destroy(m_b);
    }
    bson_t *get() const
    {
        return m_b;
    }
    explicit operator bool() const
    {
        return m_b != nullptr;
    }

private:
    bson_t *m_b;
};

/* a string as a JSON string literal */
QByteArray jsonString(const QString &s)
{
    const QByteArray array = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}

/* canonical Extended JSON in; nullptr + *error when libbson won't take it */
Bson fromJson(const QByteArray &json, QString *error)
{
    bson_error_t e;
    const QByteArray text = json.trimmed().isEmpty() ? QByteArrayLiteral("{}") : json;
    bson_t *b =
        bson_new_from_json(reinterpret_cast<const uint8_t *>(text.constData()), text.size(), &e);
    if(!b && error)
        *error = QStringLiteral("not a document: %1").arg(message(e));
    return Bson(b);
}

QByteArray toJson(const bson_t *b, bool relaxed = false)
{
    size_t len = 0;
    char *s =
        relaxed ? bson_as_relaxed_extended_json(b, &len) : bson_as_canonical_extended_json(b, &len);
    QByteArray out(s, qsizetype(len));
    bson_free(s);
    return out;
}

qint64 replyCount(const bson_t &reply, const char *field)
{
    bson_iter_t it;
    return bson_iter_init_find(&it, &reply, field) ? bson_iter_as_int64(&it) : 0;
}

/* drains a cursor into `each`; false (with *error) when the cursor failed */
bool drain(mongoc_cursor_t *cursor, const MongoClient::Each &each, QString *error, bool relaxed)
{
    const bson_t *doc = nullptr;
    bool stopped = false;
    while(!stopped && mongoc_cursor_next(cursor, &doc))
        stopped = each && !each(toJson(doc, relaxed));
    bson_error_t e;
    const bool failed = mongoc_cursor_error(cursor, &e);
    if(failed && error)
        *error = message(e);
    mongoc_cursor_destroy(cursor);
    return !failed;
}

} // namespace

struct MongoClient::Impl
{
    mongoc_uri_t *uri = nullptr;
    mongoc_client_pool_t *pool = nullptr;
    std::shared_ptr<SshTunnel> tunnel;

    /* a client of the pool's for the length of one call */
    struct Client
    {
        explicit Client(mongoc_client_pool_t *p) : pool(p), c(mongoc_client_pool_pop(p)) {}
        ~Client()
        {
            mongoc_client_pool_push(pool, c);
        }
        mongoc_client_pool_t *pool;
        mongoc_client_t *c;
    };
    struct Collection
    {
        Collection(mongoc_client_t *c, const QString &db, const QString &name)
            : coll(mongoc_client_get_collection(c, db.toUtf8().constData(),
                                                name.toUtf8().constData()))
        {}
        ~Collection()
        {
            mongoc_collection_destroy(coll);
        }
        mongoc_collection_t *coll;
    };
    struct Database
    {
        Database(mongoc_client_t *c, const QString &name)
            : db(mongoc_client_get_database(c, name.toUtf8().constData()))
        {}
        ~Database()
        {
            mongoc_database_destroy(db);
        }
        mongoc_database_t *db;
    };
};

MongoClient::MongoClient() : d(std::make_unique<Impl>()) {}

MongoClient::~MongoClient()
{
    if(d->pool)
        mongoc_client_pool_destroy(d->pool);
    if(d->uri)
        mongoc_uri_destroy(d->uri);
}

bool MongoClient::available()
{
    return true;
}

std::unique_ptr<MongoClient> MongoClient::connect(const ConnectionParams &p, QString *error)
{
    startDriver();
    std::unique_ptr<MongoClient> m(new MongoClient);
    const auto hostPort = [](const QString &host, int port) {
        return (host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']')
                                                : host) +
               QLatin1Char(':') + QString::number(port);
    };

    QString base;
    if(p.useSsh) {
        /* the tunnel reaches host:port from the SSH host; past it there is
         * one server and nothing to discover */
        QString err;
        m->d->tunnel = SshTunnel::open(p, &err);
        if(!m->d->tunnel) {
            *error = err;
            return nullptr;
        }
        base = QStringLiteral("mongodb://127.0.0.1:%1/?directConnection=true")
                   .arg(m->d->tunnel->localPort());
    } else if(!p.mongoUri.trimmed().isEmpty()) {
        base = p.mongoUri.trimmed();
    } else {
        base = QStringLiteral("mongodb://%1/").arg(hostPort(p.host, p.port > 0 ? p.port : 27017));
    }

    bson_error_t e;
    m->d->uri = mongoc_uri_new_with_error(base.toUtf8().constData(), &e);
    if(!m->d->uri) {
        *error = QStringLiteral("not a MongoDB connection string: %1").arg(message(e));
        return nullptr;
    }
    mongoc_uri_t *uri = m->d->uri;
    if(!p.user.isEmpty()) {
        mongoc_uri_set_username(uri, p.user.toUtf8().constData());
        mongoc_uri_set_password(uri, p.password.toUtf8().constData());
    }
    if(!p.authDatabase.trimmed().isEmpty())
        mongoc_uri_set_auth_source(uri, p.authDatabase.trimmed().toUtf8().constData());
    if(p.useSsl) {
        mongoc_uri_set_option_as_bool(uri, MONGOC_URI_TLS, true);
        if(!p.sslCa.isEmpty())
            mongoc_uri_set_option_as_utf8(uri, MONGOC_URI_TLSCAFILE, p.sslCa.toUtf8().constData());
        if(!p.sslCert.isEmpty())
            mongoc_uri_set_option_as_utf8(uri, MONGOC_URI_TLSCERTIFICATEKEYFILE,
                                          p.sslCert.toUtf8().constData());
    }
    /* the SQL drivers' 10 seconds, unless the connection string says */
    for(const char *option : {MONGOC_URI_SERVERSELECTIONTIMEOUTMS, MONGOC_URI_CONNECTTIMEOUTMS})
        if(!mongoc_uri_has_option(uri, option))
            mongoc_uri_set_option_as_int32(uri, option, 10000);
    if(!mongoc_uri_has_option(uri, MONGOC_URI_APPNAME))
        mongoc_uri_set_appname(uri, "OpenYog");

    m->d->pool = mongoc_client_pool_new_with_error(uri, &e);
    if(!m->d->pool) {
        *error = message(e);
        return nullptr;
    }
    m->m_address = (p.user.isEmpty() ? QString() : p.user + QLatin1Char('@')) +
                   (p.useSsh ? hostPort(p.host, p.port > 0 ? p.port : 27017) +
                                   QStringLiteral(" via SSH ") + p.sshHost
                             : base);

    /* the login happens on the first command */
    Impl::Client c(m->d->pool);
    bson_t reply;
    Bson ping = fromJson("{\"ping\": 1}", nullptr);
    const bool ok = mongoc_client_command_simple(c.c, "admin", ping.get(), nullptr, &reply, &e);
    bson_destroy(&reply);
    if(!ok) {
        *error = message(e);
        return nullptr;
    }
    Bson buildInfo = fromJson("{\"buildInfo\": 1}", nullptr);
    if(mongoc_client_command_simple(c.c, "admin", buildInfo.get(), nullptr, &reply, &e)) {
        bson_iter_t it;
        if(bson_iter_init_find(&it, &reply, "version") && BSON_ITER_HOLDS_UTF8(&it))
            m->m_version = QString::fromUtf8(bson_iter_utf8(&it, nullptr));
    }
    bson_destroy(&reply);
    return m;
}

QStringList MongoClient::databases(QString *error)
{
    Impl::Client c(d->pool);
    Bson opts = fromJson("{\"authorizedDatabases\": true}", nullptr);
    bson_error_t e;
    char **names = mongoc_client_get_database_names_with_opts(c.c, opts.get(), &e);
    if(!names) {
        if(error)
            *error = message(e);
        return {};
    }
    QStringList out;
    for(char **n = names; *n; ++n)
        out << QString::fromUtf8(*n);
    bson_strfreev(names);
    out.sort();
    return out;
}

QList<MongoClient::Collection> MongoClient::collections(const QString &db, QString *error)
{
    Impl::Client c(d->pool);
    Impl::Database database(c.c, db);
    QList<Collection> out;
    const bool ok = drain(
        mongoc_database_find_collections_with_opts(database.db, nullptr),
        [&](const QByteArray &json) {
            Bson b = fromJson(json, nullptr);
            bson_iter_t it;
            Collection col;
            if(bson_iter_init_find(&it, b.get(), "name") && BSON_ITER_HOLDS_UTF8(&it))
                col.name = QString::fromUtf8(bson_iter_utf8(&it, nullptr));
            if(bson_iter_init_find(&it, b.get(), "type") && BSON_ITER_HOLDS_UTF8(&it))
                col.type = QString::fromUtf8(bson_iter_utf8(&it, nullptr));
            if(!col.name.isEmpty())
                out << col;
            return true;
        },
        error, false);
    if(!ok)
        return {};
    std::sort(out.begin(), out.end(),
              [](const Collection &a, const Collection &b) { return a.name < b.name; });
    return out;
}

bool MongoClient::find(const QString &db, const QString &collection, const Find &f,
                       const Each &each, QString *error, bool relaxed)
{
    Bson filter = fromJson(f.filter, error);
    if(!filter)
        return false;
    Bson opts(bson_new());
    for(const auto &[name, json] : {std::pair{"projection", f.projection}, {"sort", f.sort}}) {
        if(json.trimmed().isEmpty())
            continue;
        Bson part = fromJson(json, error);
        if(!part)
            return false;
        if(!bson_empty(part.get()))
            bson_append_document(opts.get(), name, -1, part.get());
    }
    if(f.skip > 0)
        bson_append_int64(opts.get(), "skip", -1, f.skip);
    if(f.limit > 0)
        bson_append_int64(opts.get(), "limit", -1, f.limit);
    Impl::Client c(d->pool);
    Impl::Collection coll(c.c, db, collection);
    return drain(mongoc_collection_find_with_opts(coll.coll, filter.get(), opts.get(), nullptr),
                 each, error, relaxed);
}

qint64 MongoClient::count(const QString &db, const QString &collection, const QByteArray &filter,
                          QString *error)
{
    Bson f = fromJson(filter, error);
    if(!f)
        return -1;
    Impl::Client c(d->pool);
    Impl::Collection coll(c.c, db, collection);
    bson_error_t e;
    const int64_t n =
        bson_empty(f.get())
            ? mongoc_collection_estimated_document_count(coll.coll, nullptr, nullptr, nullptr, &e)
            : mongoc_collection_count_documents(coll.coll, f.get(), nullptr, nullptr, nullptr, &e);
    if(n < 0 && error)
        *error = message(e);
    return n;
}

bool MongoClient::aggregate(const QString &db, const QString &collection,
                            const QByteArray &pipeline, const Each &each, QString *error)
{
    /* libbson reads documents, not a bare array: {"pipeline": […]} */
    Bson p = fromJson("{\"pipeline\": " + pipeline + "}", error);
    if(!p)
        return false;
    Impl::Client c(d->pool);
    Impl::Collection coll(c.c, db, collection);
    return drain(
        mongoc_collection_aggregate(coll.coll, MONGOC_QUERY_NONE, p.get(), nullptr, nullptr), each,
        error, false);
}

bool MongoClient::insert(const QString &db, const QString &collection,
                         const QList<QByteArray> &documents, QString *error)
{
    if(documents.isEmpty())
        return true;
    std::vector<Bson> docs;
    for(const QByteArray &json : documents) {
        docs.push_back(fromJson(json, error));
        if(!docs.back())
            return false;
    }
    /* insert_many wants a C array of them */
    const auto ptrs =
        static_cast<const bson_t **>(bson_malloc(sizeof(const bson_t *) * docs.size()));
    for(size_t i = 0; i < docs.size(); ++i)
        ptrs[i] = docs[i].get();
    Impl::Client c(d->pool);
    Impl::Collection coll(c.c, db, collection);
    bson_t reply;
    bson_error_t e;
    const bool ok =
        mongoc_collection_insert_many(coll.coll, ptrs, docs.size(), nullptr, &reply, &e);
    bson_free(ptrs);
    bson_destroy(&reply);
    if(!ok && error)
        *error = message(e);
    return ok;
}

bool MongoClient::replace(const QString &db, const QString &collection, const QByteArray &filter,
                          const QByteArray &document, qint64 *matched, QString *error)
{
    Bson f = fromJson(filter, error);
    Bson doc = f ? fromJson(document, error) : Bson();
    if(!f || !doc)
        return false;
    Impl::Client c(d->pool);
    Impl::Collection coll(c.c, db, collection);
    bson_t reply;
    bson_error_t e;
    const bool ok =
        mongoc_collection_replace_one(coll.coll, f.get(), doc.get(), nullptr, &reply, &e);
    if(matched)
        *matched = ok ? replyCount(reply, "matchedCount") : 0;
    bson_destroy(&reply);
    if(!ok && error)
        *error = message(e);
    return ok;
}

bool MongoClient::remove(const QString &db, const QString &collection, const QByteArray &filter,
                         bool many, qint64 *deleted, QString *error)
{
    Bson f = fromJson(filter, error);
    if(!f)
        return false;
    Impl::Client c(d->pool);
    Impl::Collection coll(c.c, db, collection);
    bson_t reply;
    bson_error_t e;
    const bool ok = many ? mongoc_collection_delete_many(coll.coll, f.get(), nullptr, &reply, &e)
                         : mongoc_collection_delete_one(coll.coll, f.get(), nullptr, &reply, &e);
    if(deleted)
        *deleted = ok ? replyCount(reply, "deletedCount") : 0;
    bson_destroy(&reply);
    if(!ok && error)
        *error = message(e);
    return ok;
}

bool MongoClient::createCollection(const QString &db, const QString &collection, QString *error)
{
    Impl::Client c(d->pool);
    Impl::Database database(c.c, db);
    bson_error_t e;
    mongoc_collection_t *coll = mongoc_database_create_collection(
        database.db, collection.toUtf8().constData(), nullptr, &e);
    if(!coll) {
        if(error)
            *error = message(e);
        return false;
    }
    mongoc_collection_destroy(coll);
    return true;
}

bool MongoClient::dropCollection(const QString &db, const QString &collection, QString *error)
{
    Impl::Client c(d->pool);
    Impl::Collection coll(c.c, db, collection);
    bson_error_t e;
    const bool ok = mongoc_collection_drop_with_opts(coll.coll, nullptr, &e);
    if(!ok && error)
        *error = message(e);
    return ok;
}

bool MongoClient::dropDatabase(const QString &db, QString *error)
{
    Impl::Client c(d->pool);
    Impl::Database database(c.c, db);
    bson_error_t e;
    const bool ok = mongoc_database_drop_with_opts(database.db, nullptr, &e);
    if(!ok && error)
        *error = message(e);
    return ok;
}

bool MongoClient::indexes(const QString &db, const QString &collection, QList<QByteArray> *out,
                          QString *error)
{
    Impl::Client c(d->pool);
    Impl::Collection coll(c.c, db, collection);
    out->clear();
    return drain(
        mongoc_collection_find_indexes_with_opts(coll.coll, nullptr),
        [out](const QByteArray &json) {
            *out << json;
            return true;
        },
        error, false);
}

bool MongoClient::createIndex(const QString &db, const QString &collection, const QByteArray &keys,
                              const QString &name, bool unique, QString *error)
{
    Bson k = fromJson(keys, error);
    if(!k)
        return false;
    if(bson_empty(k.get())) {
        if(error)
            *error = QStringLiteral("an index needs at least one key");
        return false;
    }
    QString indexName = name.trimmed();
    if(indexName.isEmpty()) {
        /* the server's own naming: field_direction joined by _ */
        bson_iter_t it;
        bson_iter_init(&it, k.get());
        QStringList parts;
        while(bson_iter_next(&it)) {
            const QString value = BSON_ITER_HOLDS_UTF8(&it)
                                      ? QString::fromUtf8(bson_iter_utf8(&it, nullptr))
                                      : QString::number(bson_iter_as_int64(&it));
            parts << QString::fromUtf8(bson_iter_key(&it)) << value;
        }
        indexName = parts.join(QLatin1Char('_'));
    }
    Bson cmd = fromJson(
        "{\"createIndexes\": " + jsonString(collection) + ", \"indexes\": [{\"key\": " + keys +
            ", \"name\": " + jsonString(indexName) + (unique ? ", \"unique\": true}]}" : "}]}"),
        error);
    if(!cmd)
        return false;

    Impl::Client c(d->pool);
    Impl::Database database(c.c, db);
    bson_t reply;
    bson_error_t e;
    const bool ok =
        mongoc_database_write_command_with_opts(database.db, cmd.get(), nullptr, &reply, &e);
    bson_destroy(&reply);
    if(!ok && error)
        *error = message(e);
    return ok;
}

bool MongoClient::dropIndex(const QString &db, const QString &collection, const QString &name,
                            QString *error)
{
    Impl::Client c(d->pool);
    Impl::Collection coll(c.c, db, collection);
    bson_error_t e;
    const bool ok =
        mongoc_collection_drop_index_with_opts(coll.coll, name.toUtf8().constData(), nullptr, &e);
    if(!ok && error)
        *error = message(e);
    return ok;
}

bool MongoClient::command(const QString &db, const QByteArray &command, QByteArray *reply,
                          QString *error)
{
    Bson cmd = fromJson(command, error);
    if(!cmd)
        return false;
    Impl::Client c(d->pool);
    bson_t r;
    bson_error_t e;
    const bool ok =
        mongoc_client_command_simple(c.c, db.toUtf8().constData(), cmd.get(), nullptr, &r, &e);
    if(reply)
        *reply = ok ? toJson(&r) : QByteArray();
    bson_destroy(&r);
    if(!ok && error)
        *error = message(e);
    return ok;
}

#else /* no libmongoc */

struct MongoClient::Impl
{};

MongoClient::MongoClient() = default;
MongoClient::~MongoClient() = default;

bool MongoClient::available()
{
    return false;
}

std::unique_ptr<MongoClient> MongoClient::connect(const ConnectionParams &, QString *error)
{
    *error = QStringLiteral("MongoDB isn't available in this build (it needs libmongoc 2)");
    return nullptr;
}

#define OPENYOG_MONGO_UNAVAILABLE                                                                  \
    if(error)                                                                                      \
        *error = QStringLiteral("MongoDB isn't available in this build");

QStringList MongoClient::databases(QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return {};
}
QList<MongoClient::Collection> MongoClient::collections(const QString &, QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return {};
}
bool MongoClient::find(const QString &, const QString &, const Find &, const Each &, QString *error,
                       bool)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
qint64 MongoClient::count(const QString &, const QString &, const QByteArray &, QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return -1;
}
bool MongoClient::aggregate(const QString &, const QString &, const QByteArray &, const Each &,
                            QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
bool MongoClient::insert(const QString &, const QString &, const QList<QByteArray> &,
                         QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
bool MongoClient::replace(const QString &, const QString &, const QByteArray &, const QByteArray &,
                          qint64 *, QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
bool MongoClient::remove(const QString &, const QString &, const QByteArray &, bool, qint64 *,
                         QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
bool MongoClient::createCollection(const QString &, const QString &, QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
bool MongoClient::dropCollection(const QString &, const QString &, QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
bool MongoClient::dropDatabase(const QString &, QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
bool MongoClient::indexes(const QString &, const QString &, QList<QByteArray> *, QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
bool MongoClient::createIndex(const QString &, const QString &, const QByteArray &, const QString &,
                              bool, QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
bool MongoClient::dropIndex(const QString &, const QString &, const QString &, QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}
bool MongoClient::command(const QString &, const QByteArray &, QByteArray *, QString *error)
{
    OPENYOG_MONGO_UNAVAILABLE
    return false;
}

#endif
