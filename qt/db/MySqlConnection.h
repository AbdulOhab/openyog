/* OpenYog — MySQL/MariaDB IDbConnection implementation. Thin wrapper around
 * the mysql_* C API call sequences already used throughout qt/ before the
 * driver seam existed — behavior-preserving, not a rewrite. */
#pragma once

#include "IDbConnection.h"

#include <mysql/mysql.h>

class MySqlConnection : public IDbConnection
{
public:
    /* By default takes ownership of an already-connected handle and closes
     * it on destruction. Pass owns=false to wrap a handle owned elsewhere —
     * e.g. ConnectionTab's still-unmigrated MYSQL* m_conn, borrowed for one
     * call into an already-migrated file during the incremental seam
     * rollout (plan.md). */
    /* host/port/user/password are kept only for cancel() — it needs a
     * throwaway auxiliary connection to send KILL QUERY, since the busy
     * connection can't process any command while a query runs on it. */
    MySqlConnection(MYSQL *conn, const QString &host, int port, const QString &user,
                    const QString &password, bool owns = true);
    ~MySqlConnection() override;

    SqlDriverType driverType() const override
    {
        return SqlDriverType::Mysql;
    }

    void cancel() override;
    bool query(const QString &sql, DbResultSet *result, QString *message) override;

    bool streamQuery(const QString &sql, QString *error,
                     const std::function<void(const QStringList &headers)> &onHeaders,
                     const std::function<bool(const QVector<QByteArray> &fields,
                                              const QVector<bool> &isNull)> &onRow) override;

    QByteArray escape(const QByteArray &raw) override;
    QString quoteIdent(const QString &ident) override;
    QString lastError() override;
    qint64 affectedRows() override;
    QString serverInfo() override;
    QString info() override;

    QStringList listDatabases() override;
    QStringList listTables(const QString &db, const QString &typeFilter) override;
    DbResultSet listColumns(const QString &db, const QString &table) override;
    QStringList listAllColumnNames(const QString &db) override;
    DbResultSet listIndexes(const QString &db, const QString &table) override;
    DbResultSet listForeignKeys(const QString &db, const QString &table) override;
    QStringList listTriggers(const QString &db) override;
    DbResultSet listTableTriggers(const QString &db, const QString &table) override;
    DbResultSet listRoutines(const QString &db) override;
    QStringList listEvents(const QString &db) override;
    QString showCreate(const QString &kind, const QString &db, const QString &name,
                       QString *error) override;

    QString sqlFkChecks(bool enable) override;
    QString sqlSetNames(const QString &charset) override;
    QString sqlInsertDefaults(const QString &db, const QString &table) override;
    QString sqlTruncateTable(const QString &db, const QString &table) override;
    bool supportsLimitOnUpdateDelete() override;

private:
    /* runs `sql`, buffers the result (if any) into a DbResultSet */
    bool runBuffered(const QString &sql, DbResultSet *out, QString *error);

    MYSQL *m_conn;
    bool m_owns;
    QString m_host, m_user, m_password;
    int m_port;
};
