#include "common/net_util.h"

#include "common/util.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

namespace squeeze2raop2 {

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0 && fd_ != fd) {
        // Deliberately single-shot: see the header for why retrying close()
        // after EINTR is unsafe on Linux.
        const int rc = ::close(fd_);
        (void)rc;
    }
    fd_ = fd;
}

void UniqueFd::shutdown() noexcept {
    if (fd_ < 0) return;
    int rc;
    do {
        rc = ::shutdown(fd_, SHUT_RDWR);
    } while (rc < 0 && errno == EINTR);
}

std::string ipv4ToString(const in_addr& addr) {
    char buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr, buf, sizeof(buf))) return std::string();
    return std::string(buf);
}

std::string ipv4ToString(uint32_t hostOrder) {
    in_addr a{};
    a.s_addr = htonl(hostOrder);
    return ipv4ToString(a);
}

std::optional<in_addr> resolveIpv4(const std::string& host) {
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) == 1) return a;
    // getaddrinfo instead of gethostbyname2: the latter returns a thread-unsafe
    // static hostent, and sessions resolve concurrently.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr)
        return std::nullopt;
    std::optional<in_addr> out;
    if (res->ai_addr != nullptr && res->ai_addr->sa_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
        out = sin->sin_addr;
    }
    freeaddrinfo(res);
    return out;
}

uint16_t localPort(int fd) {
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) != 0) return 0;
    return ntohs(sa.sin_port);
}

int connectTcp(const std::string& host, uint16_t port, std::string& errorOut) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    const auto addr = resolveIpv4(host);
    if (!addr) {
        errorOut = std::string("cannot resolve ") + host;
        return -1;
    }
    sa.sin_addr = *addr;
    UniqueFd fd{::socket(AF_INET, SOCK_STREAM, 0)};
    if (!fd) {
        errorOut = std::string("socket: ") + errnoMessage(errno);
        return -1;
    }
    int one = 1;
    (void)setsockopt(fd.get(), SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        errorOut = std::string("connect: ") + errnoMessage(errno);
        return -1;  // fd closes via RAII
    }
    return fd.release();
}

int connectSocketTuned(const std::string& host, uint16_t port, std::string& errorOut) {
    const int fd = connectTcp(host, port, errorOut);
    if (fd < 0) return -1;
    // Bounded close: an abandoned connection must not hang close() forever.
    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 3;
    (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    enableTcpKeepalive(fd);
    return fd;
}

void enableTcpKeepalive(int fd) {
    if (fd < 0) return;
    // connectTcp already enables SO_KEEPALIVE but leaves the kernel's default
    // idle (2 h); tighten it so a peer that has silently vanished is turned
    // into a socket error instead of an eternally parked read. The pump's own
    // source watchdog (source-timeout-ms) covers a live-but-silent source.
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#if defined(TCP_KEEPIDLE)
    int idle = 60;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#if defined(TCP_KEEPINTVL)
    int intvl = 15;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#if defined(TCP_KEEPCNT)
    int cnt = 4;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

bool sendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            // ENOBUFS joins EAGAIN/EINTR (slimproto's send loop): the send
            // buffer can transiently fill under concurrent writers.
            if (n < 0 && (errno == EAGAIN || errno == EINTR || errno == ENOBUFS)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace squeeze2raop2
