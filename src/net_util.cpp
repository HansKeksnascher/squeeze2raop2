#include "net_util.h"

#include "util.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
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

int connectTcp(const std::string& host, uint16_t port, std::string& errorOut) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        // getaddrinfo instead of gethostbyname2: the latter returns a
        // thread-unsafe static hostent, and sessions resolve concurrently.
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            errorOut = std::string("cannot resolve ") + host;
            return -1;
        }
        const auto* ai = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
        sa.sin_addr = ai->sin_addr;
        freeaddrinfo(res);
    }
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
