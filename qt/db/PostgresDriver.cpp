#include "PostgresDriver.h"
#include "PostgresConnection.h"

#include <libpq-fe.h>

namespace {
/* libpq's conninfo string is keyword=value pairs; a value with a space,
 * quote or backslash needs single-quote wrapping with '\'' and '\\'
 * backslash-escaped inside. Quoting every value (not just ones that need
 * it) is simplest and always correct. */
QString conninfoQuote(const QString &value)
{
    QString v = value;
    v.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    v.replace(QLatin1Char('\''), QStringLiteral("\\'"));
    return QLatin1Char('\'') + v + QLatin1Char('\'');
}
} // namespace

IDbConnection *PostgresDriver::connect(const ConnectionParams &params, QString *error,
                                       bool /*localInfile*/)
{
    QStringList parts;
    parts << QStringLiteral("host=%1").arg(conninfoQuote(params.host))
          << QStringLiteral("port=%1").arg(params.port)
          << QStringLiteral("user=%1").arg(conninfoQuote(params.user))
          << QStringLiteral("dbname=%1").arg(conninfoQuote(
                 params.database.isEmpty() ? QStringLiteral("postgres") : params.database))
          << QStringLiteral("connect_timeout=10");
    if(!params.password.isEmpty())
        parts << QStringLiteral("password=%1").arg(conninfoQuote(params.password));

    /* client-cert TLS, libpq's own option names — sslmode=verify-full needs
     * a CA to verify against; fall back to "require" (encrypted, but no
     * identity check) when the user ticked "Use SSL" without picking one. */
    if(params.useSsl) {
        parts << QStringLiteral("sslmode=%1")
                     .arg(params.sslCa.isEmpty() ? QStringLiteral("require")
                                                 : QStringLiteral("verify-full"));
        if(!params.sslCa.isEmpty())
            parts << QStringLiteral("sslrootcert=%1").arg(conninfoQuote(params.sslCa));
        if(!params.sslCert.isEmpty())
            parts << QStringLiteral("sslcert=%1").arg(conninfoQuote(params.sslCert));
        if(!params.sslKey.isEmpty())
            parts << QStringLiteral("sslkey=%1").arg(conninfoQuote(params.sslKey));
    } else {
        parts << QStringLiteral("sslmode=prefer");
    }

    const QByteArray conninfo = parts.join(QLatin1Char(' ')).toUtf8();
    PGconn *conn = PQconnectdb(conninfo.constData());
    if(PQstatus(conn) != CONNECTION_OK) {
        if(error) *error = QString::fromUtf8(PQerrorMessage(conn));
        PQfinish(conn);
        return nullptr;
    }
    return new PostgresConnection(conn);
}
