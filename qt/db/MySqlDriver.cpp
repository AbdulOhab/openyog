#include "MySqlDriver.h"
#include "MySqlConnection.h"

#include <mysql/mysql.h>

/* one connect attempt; `tcp` forces TCP instead of the library's own pick */
static MYSQL *tryConnect(const ConnectionParams &params, bool localInfile, bool tcp, QString *error)
{
    MYSQL *c = mysql_init(nullptr);
    mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if(localInfile) {
        unsigned int on = 1;
        mysql_options(c, MYSQL_OPT_LOCAL_INFILE, &on);
    }
    if(params.compress)
        mysql_options(c, MYSQL_OPT_COMPRESS, nullptr);
    if(tcp) {
        unsigned int proto = MYSQL_PROTOCOL_TCP;
        mysql_options(c, MYSQL_OPT_PROTOCOL, &proto);
    }
    /* client-cert TLS: any of key/cert/ca may be empty (server-only cert
     * checking, e.g.) — mysql_ssl_set accepts nullptr for each. */
    if(params.useSsl) {
        const QByteArray key = params.sslKey.toUtf8();
        const QByteArray cert = params.sslCert.toUtf8();
        const QByteArray ca = params.sslCa.toUtf8();
        mysql_ssl_set(c, key.isEmpty() ? nullptr : key.constData(),
                      cert.isEmpty() ? nullptr : cert.constData(),
                      ca.isEmpty() ? nullptr : ca.constData(), nullptr, nullptr);
    }
    if(!mysql_real_connect(c, params.host.toUtf8().constData(), params.user.toUtf8().constData(),
                           params.password.toUtf8().constData(),
                           params.database.isEmpty() ? nullptr
                                                     : params.database.toUtf8().constData(),
                           params.port, nullptr, 0)) {
        if(error)
            *error = QString::fromUtf8(mysql_error(c));
        mysql_close(c);
        return nullptr;
    }
    return c;
}

IDbConnection *MySqlDriver::connect(const ConnectionParams &params, QString *error,
                                    bool localInfile)
{
    /* Host "localhost" makes libmysql/libmariadb use the local UNIX socket
     * and ignore the port entirely — so a MariaDB in a container, or on any
     * port other than the socket's own server, was "not connectable" by
     * localhost:PORT while 127.0.0.1:PORT worked. A non-default port means
     * the user is aiming at TCP, so try that first; the socket stays the
     * fallback (and the first choice on the default port, where a
     * socket-only local server is the usual setup). */
    const bool isLocalhost =
        params.host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0;
    const bool tcpFirst = isLocalhost && params.port != 0 && params.port != 3306;
    QString firstError;
    MYSQL *c = tryConnect(params, localInfile, tcpFirst, &firstError);
    if(!c && isLocalhost) {
        QString secondError;
        c = tryConnect(params, localInfile, !tcpFirst, &secondError);
        if(!c && error)
            *error = tcpFirst ? firstError : secondError; /* the TCP attempt's message */
        if(!c)
            return nullptr;
    } else if(!c) {
        if(error)
            *error = firstError;
        return nullptr;
    }
    /* "Session Idle Timeout" in the connect dialog: how long the *server*
     * lets this connection sit idle before dropping it — set as a session
     * variable post-connect, there's no mysql_real_connect() option for it.
     * Best-effort: a server that rejects the SET (e.g. no SUPER for a very
     * large value) just keeps its own default, not worth failing over. */
    if(params.idleTimeoutSecs > 0) {
        const QByteArray sql =
            QStringLiteral("SET SESSION wait_timeout=%1, SESSION interactive_timeout=%1")
                .arg(params.idleTimeoutSecs)
                .toUtf8();
        mysql_query(c, sql.constData());
    }
    return new MySqlConnection(c, params.host, params.port, params.user, params.password);
}

void MySqlDriver::libraryInit()
{
    mysql_library_init(0, nullptr, nullptr);
}
void MySqlDriver::libraryShutdown()
{
    mysql_library_end();
}
