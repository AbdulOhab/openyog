#include "SshTunnel.h"

#ifdef OPENYOG_HAVE_SSH

#include <QByteArray>
#include <QDir>

#include <libssh/libssh.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <thread>
#include <vector>

namespace {

QString sshError(ssh_session s)
{
    return QString::fromUtf8(ssh_get_error(s));
}

/* a connected (not yet authenticated) session; nullptr + *error on failure.
 * ssh_connect() also applies ~/.ssh/config for the host. */
ssh_session connectSession(const ConnectionParams &p, QString *error)
{
    ssh_session s = ssh_new();
    if(!s) {
        *error = QStringLiteral("could not create an SSH session");
        return nullptr;
    }
    const QByteArray host = p.sshHost.toUtf8();
    const QByteArray user = p.sshUser.toUtf8();
    unsigned int port = p.sshPort > 0 ? unsigned(p.sshPort) : 22u;
    long timeout = 10; /* seconds, as the database drivers' own */
    ssh_options_set(s, SSH_OPTIONS_HOST, host.constData());
    ssh_options_set(s, SSH_OPTIONS_PORT, &port);
    if(!user.isEmpty())
        ssh_options_set(s, SSH_OPTIONS_USER, user.constData());
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
    /* OPENYOG_SSH_DIR: known_hosts and default keys from that directory
     * instead of ~/.ssh, and no ~/.ssh/config — keeps the selftests off the
     * user's real ones (libssh finds the home dir through passwd, so
     * overriding HOME isn't enough) */
    const QByteArray sshDir = qgetenv("OPENYOG_SSH_DIR");
    if(!sshDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(sshDir));
        const QByteArray knownHosts = sshDir + "/known_hosts";
        bool processConfig = false;
        ssh_options_set(s, SSH_OPTIONS_SSH_DIR, sshDir.constData());
        ssh_options_set(s, SSH_OPTIONS_KNOWNHOSTS, knownHosts.constData());
        ssh_options_set(s, SSH_OPTIONS_PROCESS_CONFIG, &processConfig);
    }
    if(ssh_connect(s) != SSH_OK) {
        *error = QStringLiteral("SSH connection to %1:%2 failed: %3")
                     .arg(p.sshHost)
                     .arg(port)
                     .arg(sshError(s));
        ssh_free(s);
        return nullptr;
    }
    return s;
}

void closeSession(ssh_session s)
{
    if(!s)
        return;
    ssh_disconnect(s);
    ssh_free(s);
}

SshTunnel::HostKey inspectHostKey(ssh_session s)
{
    SshTunnel::HostKey hk;
    ssh_key key = nullptr;
    if(ssh_get_server_publickey(s, &key) != SSH_OK) {
        hk.error = sshError(s);
        return hk;
    }
    unsigned char *hash = nullptr;
    size_t len = 0;
    if(ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &len) == SSH_OK) {
        char *fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, len);
        hk.fingerprint = QString::fromUtf8(fp);
        ssh_string_free_char(fp);
        ssh_clean_pubkey_hash(&hash);
    }
    ssh_key_free(key);

    switch(ssh_session_is_known_server(s)) {
        case SSH_KNOWN_HOSTS_OK:
            hk.state = SshTunnel::HostKeyState::Known;
            break;
        case SSH_KNOWN_HOSTS_CHANGED:
        case SSH_KNOWN_HOSTS_OTHER: /* a key of another type is on record */
            hk.state = SshTunnel::HostKeyState::Changed;
            break;
        case SSH_KNOWN_HOSTS_NOT_FOUND: /* no known_hosts file yet */
        case SSH_KNOWN_HOSTS_UNKNOWN:
            hk.state = SshTunnel::HostKeyState::Unknown;
            break;
        case SSH_KNOWN_HOSTS_ERROR:
            hk.error = sshError(s);
            break;
    }
    return hk;
}

/* key file (passphrase = sshPassword) if one is set; otherwise the password
 * if one is set, then ssh-agent / the default ~/.ssh keys */
bool authenticate(ssh_session s, const ConnectionParams &p, QString *error)
{
    const QByteArray secret = p.sshPassword.toUtf8();
    if(!p.sshKeyFile.isEmpty()) {
        ssh_key key = nullptr;
        if(ssh_pki_import_privkey_file(p.sshKeyFile.toUtf8().constData(),
                                       secret.isEmpty() ? nullptr : secret.constData(), nullptr,
                                       nullptr, &key) != SSH_OK) {
            *error = QStringLiteral("could not read the SSH private key %1 (wrong passphrase?)")
                         .arg(p.sshKeyFile);
            return false;
        }
        const int rc = ssh_userauth_publickey(s, nullptr, key);
        ssh_key_free(key);
        if(rc == SSH_AUTH_SUCCESS)
            return true;
        *error = QStringLiteral("the SSH server refused the key %1").arg(p.sshKeyFile);
        return false;
    }
    /* a given password's own failure is the one worth reporting, not the
     * agent/default-key fallback's that runs after it */
    QString passwordError;
    if(!secret.isEmpty()) {
        if(ssh_userauth_password(s, nullptr, secret.constData()) == SSH_AUTH_SUCCESS)
            return true;
        passwordError = QStringLiteral("wrong password, or the server doesn't take passwords (%1)")
                            .arg(sshError(s));
    }
    if(ssh_userauth_publickey_auto(s, nullptr, nullptr) == SSH_AUTH_SUCCESS)
        return true;
    *error = QStringLiteral("SSH login as %1@%2 failed: %3")
                 .arg(p.sshUser, p.sshHost,
                      !passwordError.isEmpty()
                          ? passwordError
                          : QStringLiteral("no password given, and no ssh-agent or ~/.ssh key "
                                           "was accepted"));
    return false;
}

/* one forwarded connection: the local socket the database client opened,
 * and its channel */
struct Pipe
{
    int fd = -1;
    ssh_channel channel = nullptr;
    QByteArray toSocket; /* channel data the socket couldn't take yet */
    bool socketEof = false;
};

void closePipe(Pipe &p)
{
    if(p.channel) {
        ssh_channel_close(p.channel);
        ssh_channel_free(p.channel);
        p.channel = nullptr;
    }
    if(p.fd >= 0) {
        ::close(p.fd);
        p.fd = -1;
    }
}

} // namespace

struct SshTunnel::Impl
{
    ssh_session session = nullptr;
    int listenFd = -1;
    int localPort = 0;
    int wake[2] = {-1, -1}; /* the destructor writes here to stop the worker */
    QByteArray remoteHost;
    int remotePort = 0;
    std::vector<Pipe> pipes;
    std::thread worker;
    std::atomic<bool> stopping{false};

    bool openChannel(Pipe &p)
    {
        p.channel = ssh_channel_new(session);
        if(!p.channel)
            return false;
        if(ssh_channel_open_forward(p.channel, remoteHost.constData(), remotePort, "127.0.0.1",
                                    localPort) != SSH_OK) {
            ssh_channel_free(p.channel);
            p.channel = nullptr;
            return false;
        }
        return true;
    }

    /* everything that's ready, in both directions; false when this pipe is
     * finished */
    bool pump(Pipe &p, short revents)
    {
        char buf[16384];
        if(revents & (POLLIN | POLLHUP | POLLERR)) {
            const ssize_t n = ::read(p.fd, buf, sizeof buf);
            if(n > 0) {
                if(ssh_channel_write(p.channel, buf, uint32_t(n)) == SSH_ERROR)
                    return false;
            } else if(n == 0 || (errno != EAGAIN && errno != EINTR)) {
                p.socketEof = true;
                ssh_channel_send_eof(p.channel);
            }
        }
        for(;;) {
            const int n = ssh_channel_read_nonblocking(p.channel, buf, sizeof buf, 0);
            if(n == SSH_ERROR)
                return false;
            if(n <= 0)
                break;
            p.toSocket.append(buf, n);
        }
        while(!p.toSocket.isEmpty()) {
            const ssize_t w =
                ::send(p.fd, p.toSocket.constData(), size_t(p.toSocket.size()), MSG_NOSIGNAL);
            if(w > 0)
                p.toSocket.remove(0, int(w));
            else if(w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                break;
            else
                return false;
        }
        if(ssh_channel_is_eof(p.channel) && p.toSocket.isEmpty())
            return false; /* the server side finished: let the client see it */
        return !(p.socketEof && ssh_channel_is_closed(p.channel));
    }

    void run()
    {
        while(!stopping) {
            std::vector<pollfd> fds;
            fds.push_back({wake[0], POLLIN, 0});
            fds.push_back({listenFd, POLLIN, 0});
            fds.push_back({ssh_get_fd(session), POLLIN, 0});
            for(const Pipe &p : pipes)
                fds.push_back(
                    {p.fd,
                     short(p.socketEof ? 0 : POLLIN) | short(p.toSocket.isEmpty() ? 0 : POLLOUT),
                     0});
            if(::poll(fds.data(), nfds_t(fds.size()), 200) < 0 && errno != EINTR)
                break;
            if(fds[0].revents)
                break;
            /* the session's own fd being readable may carry data for any
             * channel, so every pipe gets a look each round (fds[3 + i]
             * is pipes[i]'s, kept in step when one is dropped) */
            for(size_t i = 0; i < pipes.size();) {
                if(pump(pipes[i], fds[3 + i].revents)) {
                    ++i;
                } else {
                    closePipe(pipes[i]);
                    pipes.erase(pipes.begin() + long(i));
                    fds.erase(fds.begin() + long(3 + i));
                }
            }
            /* new local connections last: they have no slot in fds yet */
            if(fds[1].revents & POLLIN) {
                const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if(fd >= 0) {
                    Pipe p;
                    p.fd = fd;
                    if(openChannel(p))
                        pipes.push_back(p);
                    else
                        ::close(fd);
                }
            }
        }
    }
};

SshTunnel::SshTunnel() : d(std::make_unique<Impl>()) {}

SshTunnel::~SshTunnel()
{
    if(d->worker.joinable()) {
        d->stopping = true;
        const char c = 0;
        [[maybe_unused]] const ssize_t ignored = ::write(d->wake[1], &c, 1);
        d->worker.join();
    }
    for(Pipe &p : d->pipes)
        closePipe(p);
    for(int fd : {d->listenFd, d->wake[0], d->wake[1]})
        if(fd >= 0)
            ::close(fd);
    closeSession(d->session);
}

bool SshTunnel::available()
{
    return true;
}

int SshTunnel::localPort() const
{
    return d->localPort;
}

SshTunnel::HostKey SshTunnel::checkHostKey(const ConnectionParams &params)
{
    QString error;
    ssh_session s = connectSession(params, &error);
    if(!s) {
        HostKey hk;
        hk.error = error;
        return hk;
    }
    const HostKey hk = inspectHostKey(s);
    closeSession(s);
    return hk;
}

bool SshTunnel::trustHostKey(const ConnectionParams &params, QString *error)
{
    ssh_session s = connectSession(params, error);
    if(!s)
        return false;
    const HostKey hk = inspectHostKey(s);
    bool ok = false;
    if(hk.state == HostKeyState::Changed)
        *error = QStringLiteral("the SSH host key of %1 has changed — refusing to trust it")
                     .arg(params.sshHost);
    else if(hk.state == HostKeyState::Error)
        *error = hk.error;
    else if(hk.state == HostKeyState::Known || ssh_session_update_known_hosts(s) == SSH_OK)
        ok = true;
    else
        *error = QStringLiteral("could not write known_hosts: %1").arg(sshError(s));
    closeSession(s);
    return ok;
}

std::shared_ptr<SshTunnel> SshTunnel::open(const ConnectionParams &params, QString *error)
{
    QString err;
    std::shared_ptr<SshTunnel> t(new SshTunnel);
    Impl &d = *t->d;
    d.session = connectSession(params, &err);
    if(!d.session) {
        *error = err;
        return nullptr;
    }
    const HostKey hk = inspectHostKey(d.session);
    if(hk.state != HostKeyState::Known) {
        *error = hk.state == HostKeyState::Changed
                     ? QStringLiteral("the SSH host key of %1 has changed (%2) — refusing to "
                                      "connect; if that's expected, remove its old entry from "
                                      "~/.ssh/known_hosts")
                           .arg(params.sshHost, hk.fingerprint)
                 : hk.state == HostKeyState::Unknown
                     ? QStringLiteral("the SSH host key of %1 (%2) isn't trusted yet")
                           .arg(params.sshHost, hk.fingerprint)
                     : hk.error;
        return nullptr;
    }
    if(!authenticate(d.session, params, &err)) {
        *error = err;
        return nullptr;
    }

    d.remoteHost = params.host.toUtf8();
    d.remotePort = params.port;
    d.listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* any free port */
    socklen_t len = sizeof addr;
    if(d.listenFd < 0 ||
       ::bind(d.listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 ||
       ::listen(d.listenFd, 8) != 0 ||
       ::getsockname(d.listenFd, reinterpret_cast<sockaddr *>(&addr), &len) != 0 ||
       ::pipe2(d.wake, O_CLOEXEC) != 0) {
        *error = QStringLiteral("could not open the tunnel's local port");
        return nullptr;
    }
    d.localPort = ntohs(addr.sin_port);

    /* try one channel now, so a server that forbids forwarding or can't
     * reach the database says so here rather than as a dropped connection */
    Pipe probe;
    if(!d.openChannel(probe)) {
        *error = QStringLiteral("the SSH host refused to forward to %1:%2: %3")
                     .arg(params.host)
                     .arg(params.port)
                     .arg(sshError(d.session));
        return nullptr;
    }
    closePipe(probe);

    d.worker = std::thread([&d] { d.run(); });
    return t;
}

#else /* no libssh, or not POSIX */

struct SshTunnel::Impl
{};

SshTunnel::SshTunnel() = default;
SshTunnel::~SshTunnel() = default;

bool SshTunnel::available()
{
    return false;
}

int SshTunnel::localPort() const
{
    return 0;
}

SshTunnel::HostKey SshTunnel::checkHostKey(const ConnectionParams &)
{
    HostKey hk;
    hk.error = QStringLiteral("SSH tunnels aren't available in this build");
    return hk;
}

bool SshTunnel::trustHostKey(const ConnectionParams &, QString *error)
{
    *error = QStringLiteral("SSH tunnels aren't available in this build");
    return false;
}

std::shared_ptr<SshTunnel> SshTunnel::open(const ConnectionParams &, QString *error)
{
    *error = QStringLiteral("SSH tunnels aren't available in this build");
    return nullptr;
}

#endif
