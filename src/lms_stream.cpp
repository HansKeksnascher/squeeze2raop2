#include "lms_stream.h"

#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <thread>

namespace squeeze2raop2 {

namespace {

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
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        errorOut = "socket() failed";
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 3;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        errorOut = std::string("connect: ") + errnoMessage(errno);
        ::close(fd);
        return -1;
    }
    return fd;
} // namespace

bool sendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
} // namespace squeeze2raop2

} // namespace

HttpStreamReader::~HttpStreamReader() { close(); }

void HttpStreamReader::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    headers_.clear();
    leftover_.clear();
    metaInterval_ = 0;
    metaCountdown_ = 0;
    metaBytesLeft_ = 0;
    metaBuf_.clear();
}

namespace {

// Case-insensitive header scan for "icy-metaint: N" (squeezelite stream.c).
uint32_t parseIcyMetaint(const std::string& headers) {
    std::string lower;
    lower.reserve(headers.size());
    for (char c : headers) lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    size_t p = lower.find("icy-metaint:");
    if (p == std::string::npos) return 0;
    p += sizeof("icy-metaint:") - 1;
    while (p < lower.size() && (lower[p] == ' ' || lower[p] == '\t')) ++p;
    uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(lower.data() + p, lower.data() + lower.size(), v);
    // Absurd interval (or unparseable): treat as absent.
    if (ec != std::errc{} || v == 0 || v > (1u << 20)) return 0;
    return static_cast<uint32_t>(v);
} // namespace

} // namespace

bool HttpStreamReader::openBlocking(const std::string& host, uint16_t port,
                                    const std::string& request, std::string& errorOut) {
    close();
    fd_ = connectTcp(host, port, errorOut);
    if (fd_ < 0) return false;
    std::string wire = request;
    if (wire.find("\r\n\r\n") == std::string::npos) {
        if (wire.size() < 4 || wire.compare(wire.size() - 2, 2, "\r\n") != 0) wire += "\r\n";
        wire += "\r\n";
    }
    if (!sendAll(fd_, wire.data(), wire.size())) {
        errorOut = "request send failed";
        close();
        return false;
    }
    std::string buf;
    while (true) {
        char raw[4096];
        ssize_t n = ::recv(fd_, raw, sizeof(raw), 0);
        if (n <= 0) {
            errorOut = n < 0 ? std::string("header recv: ") + errnoMessage(errno)
                             : "closed while reading headers";
            close();
            return false;
        }
        buf.append(raw, static_cast<size_t>(n));
        size_t pos = buf.find("\r\n\r\n");
        if (pos == std::string::npos) {
            if (buf.size() > 65536) {
                errorOut = "headers too big";
                close();
                return false;
            }
            continue;
        }
        headers_ = buf.substr(0, pos);
        leftover_ = buf.substr(pos + 4);
        metaInterval_ = parseIcyMetaint(headers_);
        metaCountdown_ = metaInterval_;
        if (metaInterval_) log::info("icy metadata active, interval={}", metaInterval_);
        return true;
    }
}

// Pull raw bytes from leftover_/socket: >0 = bytes, 0 = no data yet (timeout),
// -1 = socket error, -2 = orderly EOF.
ssize_t HttpStreamReader::pullRaw(std::span<char> dst, uint32_t timeoutMs) {
    if (!leftover_.empty()) {
        size_t n = std::min(leftover_.size(), dst.size());
        leftover_.copy(dst.data(), n);
        leftover_.erase(0, n);
        return static_cast<ssize_t>(n);
    }
    if (fd_ < 0) return -1;
    pollfd pfd{fd_, POLLIN, 0};
    if (poll(&pfd, 1, static_cast<int>(timeoutMs)) <= 0) return 0;
    ssize_t n = ::recv(fd_, dst.data(), dst.size(), 0);
    if (n > 0) return n;
    if (n == 0) return -2;
    if (errno == EAGAIN || errno == EINTR) return 0;
    return -1;
}

HttpStreamReader::StreamRead HttpStreamReader::read(std::span<char> buffer,
                                                    uint32_t timeoutMs) {
    size_t produced = 0;
    while (produced < buffer.size()) {
        const uint32_t timeout = produced ? 0 : timeoutMs;
        if (metaBytesLeft_) {
            char tmp[4096];
            const size_t want = std::min<size_t>(metaBytesLeft_, sizeof(tmp));
            ssize_t n = pullRaw(std::span{tmp}.first(want), timeout);
            if (n < 0) {
                if (produced) break;
                return {n == -2 ? ReadResult::AtEof : ReadResult::Closed};
            }
            if (n == 0) break;
            metaBuf_.append(tmp, static_cast<size_t>(n));
            metaBytesLeft_ -= static_cast<uint32_t>(n);
            if (!metaBytesLeft_) {
                if (metaCb_) metaCb_(metaBuf_.data(), metaBuf_.size());
                metaBuf_.clear();
                metaCountdown_ = metaInterval_;
            }
            continue;
        }
        if (metaInterval_ && !metaCountdown_) {
            char lenByte;
            const ssize_t n = pullRaw(std::span{&lenByte, 1}, timeout);
            if (n < 0) {
                if (produced) break;
                return {n == -2 ? ReadResult::AtEof : ReadResult::Closed};
            }
            if (n == 0) break;
            metaBytesLeft_ = static_cast<uint32_t>(static_cast<unsigned char>(lenByte)) * 16;
            metaBuf_.clear();
            if (!metaBytesLeft_) metaCountdown_ = metaInterval_;   // empty block, restart
            continue;
        }
        size_t want = buffer.size() - produced;
        if (metaCountdown_) want = std::min<size_t>(want, metaCountdown_);
        const ssize_t n = pullRaw(std::span{buffer}.subspan(produced, want), timeout);
        if (n < 0) {
            if (produced) break;
            return {n == -2 ? ReadResult::AtEof : ReadResult::Closed};
        }
        if (n == 0) break;
        if (metaCountdown_) metaCountdown_ -= static_cast<uint32_t>(n);
        produced += static_cast<size_t>(n);
    }
    if (produced) return {ReadResult::Data, produced};
    return {ReadResult::Closed};
}

}
