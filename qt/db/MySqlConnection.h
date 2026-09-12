/* OpenYog — MySQL/MariaDB IDbConnection implementation. Thin wrapper around
 * the mysql_* C API call sequences already used throughout qt/ before the
 * driver seam existed — behavior-preserving, not a rewrite. */
#pragma once

#include "IDbConnection.h"

#include <mysql/mysql.h>

class MySqlConnection : public IDbConnection
{
public:
    /* Takes ownership of an already-connected handle; closes it on destruction. */
    explicit MySqlConnection(MYSQL *conn);
    ~MySqlConnection() override;

    bool query(const QString &sql, DbResultSet *result, QString *message) override;

    bool streamQuery(
        const QString &sql, QString *error,
        const std::function<void(const QStringList &headers)> &onHeaders,
        const std::function<bool(const QVector<QByteArray> &fields,
                                  const QVector<bool> &isNull)> &onRow) override;

    QByteArray escape(const QByteArray &raw) override;
    QString    quoteIdent(const QString &ident) override;
    QString    lastError() override;
    qint64     affectedRows() override;
    QString    serverInfo() override;

    QStringList listDatabases() override;
    QStringList listTables(const QString &db, const QString &typeFilter) override;
    DbResultSet listColumns(const QString &db, const QString &table) override;
    DbResultSet listIndexes(const QString &db, const QString &table) override;
    DbResultSet listForeignKeys(const QString &db, const QString &table) override;
    QStringList listTriggers(const QString &db) override;
    DbResultSet listRoutines(const QString &db) override;
    QString     showCreate(const QString &kind, const QString &db,
                           const QString &name, QString *error) override;

private:
    /* runs `sql`, buffers the result (if any) into a DbResultSet */
    bool runBuffered(const QString &sql, DbResultSet *out, QString *error);

    MYSQL *m_conn;
};
