/* OpenYog — a MongoDB server, through libmongoc: what the Mongo tab reads
 * and changes.
 *
 * Documents cross this interface as canonical Extended JSON (MongoJson
 * reads and writes it), so nothing outside sees libbson. Every call takes a
 * client of its own from a mongoc_client_pool, so calls may run on worker
 * threads, several at once.
 *
 * The connection comes from ConnectionParams: a connection string
 * (mongodb:// or mongodb+srv://) when one is given, otherwise host and
 * port; user and password, with the auth database as authSource; TLS from
 * the SSL settings (CA file, client certificate with its key); and an SSH
 * tunnel like the SQL connections', through which it connects directly to
 * that one server.
 *
 * Built only with libmongoc 2 (OPENYOG_HAVE_MONGO); otherwise available()
 * is false and connect() says why. */
#pragma once

#include "../ConnectionParams.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class SshTunnel;

class MongoClient
{
public:
    static bool available();
    /* logs in and pings the server; nullptr + *error on failure */
    static std::unique_ptr<MongoClient> connect(const ConnectionParams &params, QString *error);
    ~MongoClient();
    MongoClient(const MongoClient &) = delete;
    MongoClient &operator=(const MongoClient &) = delete;

    QString serverVersion() const
    {
        return m_version;
    }
    /* where it connected: the connection string without its password */
    QString address() const
    {
        return m_address;
    }

    /* the databases the user may see (listDatabases with
     * authorizedDatabases), sorted */
    QStringList databases(QString *error);
    struct Collection
    {
        QString name;
        QString type; /* collection, view, timeseries */
    };
    QList<Collection> collections(const QString &db, QString *error);

    /* filter, projection and sort as canonical Extended JSON documents;
     * limit 0: all of them */
    struct Find
    {
        QByteArray filter = "{}";
        QByteArray projection;
        QByteArray sort;
        qint64 skip = 0;
        qint64 limit = 0;
    };
    /* each document as canonical Extended JSON (relaxed when asked), handed
     * to `each` as it arrives; `each` returning false stops it */
    using Each = std::function<bool(const QByteArray &document)>;
    bool find(const QString &db, const QString &collection, const Find &find, const Each &each,
              QString *error, bool relaxed = false);
    /* documents matching `filter`; an estimate (from the collection's
     * metadata) when the filter is empty. -1 on error */
    qint64 count(const QString &db, const QString &collection, const QByteArray &filter,
                 QString *error);
    /* `pipeline`: canonical Extended JSON of an array of stages */
    bool aggregate(const QString &db, const QString &collection, const QByteArray &pipeline,
                   const Each &each, QString *error);

    bool insert(const QString &db, const QString &collection, const QList<QByteArray> &documents,
                QString *error);
    /* replaces the one document `filter` matches; *matched says whether
     * one did */
    bool replace(const QString &db, const QString &collection, const QByteArray &filter,
                 const QByteArray &document, qint64 *matched, QString *error);
    bool remove(const QString &db, const QString &collection, const QByteArray &filter, bool many,
                qint64 *deleted, QString *error);

    bool createCollection(const QString &db, const QString &collection, QString *error);
    bool dropCollection(const QString &db, const QString &collection, QString *error);
    bool dropDatabase(const QString &db, QString *error);

    /* each index's document ({v, key, name, unique…}) */
    bool indexes(const QString &db, const QString &collection, QList<QByteArray> *out,
                 QString *error);
    /* `keys` like {name: 1, age: -1}; an empty name: the server's own
     * (name_1_age_-1) */
    bool createIndex(const QString &db, const QString &collection, const QByteArray &keys,
                     const QString &name, bool unique, QString *error);
    bool dropIndex(const QString &db, const QString &collection, const QString &name,
                   QString *error);

    /* a database command ({collStats: "x"}, {dbStats: 1}…) and its reply */
    bool command(const QString &db, const QByteArray &command, QByteArray *reply, QString *error);

    struct Impl;

private:
    MongoClient();
    std::unique_ptr<Impl> d;
    QString m_version, m_address;
};
