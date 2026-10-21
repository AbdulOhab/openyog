/* OpenYog — an SSH tunnel under a MySQL/PostgreSQL connection.
 *
 * libssh logs in to ConnectionParams::sshHost and the tunnel listens on an
 * ephemeral port on 127.0.0.1; every local connection the database client
 * makes there becomes a direct-tcpip channel to params.host:params.port as
 * seen from the SSH host. A worker thread owns the libssh session after
 * login and pumps the data both ways; the tunnel lives exactly as long as
 * the IDbConnection holding it.
 *
 * Host keys follow OpenSSH's known_hosts (~/.ssh/known_hosts, ~/.ssh/config
 * honoured): open() only proceeds for a host whose key is already known.
 * The UI asks first — checkHostKey(), and trustHostKey() once the user has
 * compared the fingerprint — and a changed key is always refused.
 *
 * Built only with libssh on a POSIX system (OPENYOG_HAVE_SSH); otherwise
 * available() is false and open() fails with an explanation. */
#pragma once

#include "../ConnectionParams.h"

#include <QString>

#include <memory>

class SshTunnel
{
public:
    enum class HostKeyState { Known, Unknown, Changed, Error };
    struct HostKey
    {
        HostKeyState state = HostKeyState::Error;
        QString fingerprint; /* "SHA256:…", as ssh-keygen -l shows it */
        QString error;
    };

    static bool available();
    /* connect to the SSH host and compare its key with known_hosts */
    static HostKey checkHostKey(const ConnectionParams &params);
    /* add the host's current key to known_hosts (not when it changed) */
    static bool trustHostKey(const ConnectionParams &params, QString *error);
    /* log in and start forwarding; nullptr + *error on failure */
    static std::shared_ptr<SshTunnel> open(const ConnectionParams &params, QString *error);

    ~SshTunnel();
    SshTunnel(const SshTunnel &) = delete;
    SshTunnel &operator=(const SshTunnel &) = delete;

    /* the 127.0.0.1 port the database client connects to */
    int localPort() const;

    struct Impl;

private:
    SshTunnel();
    std::unique_ptr<Impl> d;
};
