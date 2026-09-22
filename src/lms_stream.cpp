#include "lms_stream.h"

#include "log.h"
#include "net_util.h"
#include "util.h"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>

namespace squeeze2raop2 {

HttpStreamReader::~HttpStreamReader() { close(); }

int HttpStreamReader::fd() const {
    std::lock_guard<std::mutex> lock(fdMutex_);
    return fd_.get();
}

void HttpStreamReader::interrupt() {
    std::lock_guard<std::mutex> lock(fdMutex_);
    fd_.shutdown();
}

void HttpStreamReader::resetLocked() {
    {
        std::lock_guard<std::mutex> lock(fdMutex_);
        fd_.reset();
    }
    headers_.clear();
    leftover_.clear();
    metaInterval_ = 0;
    metaCountdown_ = 0;
    metaBytesLeft_ = 0;
    metaBuf_.clear();
}

void HttpStreamReader::close() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    resetLocked();
}

namespace {

// Case-insensitive header scan for "icy-metaint: N" (squeezelite stream.c).
uint32_t parseIcyMetaint(const std::string& headers) {
    std::string lower;
    lower.reserve(headers.size());
    for (char c : headers)
        lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    size_t p = lower.find("icy-metaint:");
    if (p == std::string::npos) return 0;
    p += sizeof("icy-metaint:") - 1;
    while (p < lower.size() && (lower[p] == ' ' || lower[p] == '\t')) ++p;
    uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(lower.data() + p, lower.data() + lower.size(), v);
    // Absurd interval (or unparseable): treat as absent.
    if (ec != std::errc{} || v == 0 || v > (1u << 20)) return 0;
    return static_cast<uint32_t>(v);
}

}  // namespace

bool HttpStreamReader::openBlocking(const std::string& host, uint16_t port,
                                    const std::string& request, std::string& errorOut) {
    // Serialize against a concurrent close() (PlayerSession::stop() racing a
    // strm s). Held for the whole header phase; interrupt() still works because
    // it does not need this lock.
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    resetLocked();
    const int raw = connectTcp(host, port, errorOut);
    if (raw < 0) return false;
    {
        std::lock_guard<std::mutex> lock(fdMutex_);
        fd_.reset(raw);
    }
    // Bounded close: an abandoned connection must not hang close() forever.
    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 3;
    (void)setsockopt(raw, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    std::string wire = request;
    if (wire.find("\r\n\r\n") == std::string::npos) {
        if (wire.size() < 4 || wire.compare(wire.size() - 2, 2, "\r\n") != 0) wire += "\r\n";
        wire += "\r\n";
    }
    if (!sendAll(raw, wire.data(), wire.size())) {
        errorOut = "request send failed";
        resetLocked();
        return false;
    }

    // Bound the header phase: a server that accepts the connection and then
    // stalls must not wedge the stream thread (and thus stopPlayback) forever.
    constexpr uint64_t kHeaderDeadlineMs = 10000;
    const uint64_t deadline = nowMs() + kHeaderDeadlineMs;
    std::string buf;
    while (true) {
        const uint64_t now = nowMs();
        if (now >= deadline) {
            errorOut = "header read timed out";
            resetLocked();
            return false;
        }
        pollfd pfd{raw, POLLIN, 0};
        const int wait = static_cast<int>(std::min<uint64_t>(1000, deadline - now));
        const int pr = ::poll(&pfd, 1, wait);
        if (pr < 0) {
            if (errno == EINTR) continue;
            errorOut = std::string("header poll: ") + errnoMessage(errno);
            resetLocked();
            return false;
        }
        if (pr == 0) continue;

        char chunk[4096];
        const ssize_t n = ::recv(raw, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            errorOut = n < 0 ? std::string("header recv: ") + errnoMessage(errno)
                             : "closed while reading headers";
            resetLocked();
            return false;
        }
        buf.append(chunk, static_cast<size_t>(n));
        const size_t pos = buf.find("\r\n\r\n");
        if (pos == std::string::npos) {
            if (buf.size() > 65536) {
                errorOut = "headers too big";
                resetLocked();
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
    const int fd = this->fd();
    if (fd < 0) return -1;
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, static_cast<int>(timeoutMs)) <= 0) return 0;
    ssize_t n = ::recv(fd, dst.data(), dst.size(), 0);
    if (n > 0) return n;
    if (n == 0) return -2;
    if (errno == EAGAIN || errno == EINTR) return 0;
    return -1;
}

HttpStreamReader::StreamRead HttpStreamReader::read(std::span<char> buffer, uint32_t timeoutMs) {
    size_t produced = 0;
    // What a zero-progress return means: a read timeout, unless a pull
    // reported EOF or a socket error first (with partial data produced,
    // the Data return wins and the close/error surfaces on the next call).
    ReadResult outcome = ReadResult::Timeout;
    while (produced < buffer.size()) {
        const uint32_t timeout = produced ? 0 : timeoutMs;
        if (metaBytesLeft_) {
            char tmp[4096];
            const size_t want = std::min<size_t>(metaBytesLeft_, sizeof(tmp));
            ssize_t n = pullRaw(std::span{tmp}.first(want), timeout);
            if (n < 0) {
                outcome = (n == -2) ? ReadResult::AtEof : ReadResult::Closed;
                if (produced) break;
                return {outcome};
            }
            if (n == 0) break;
            metaBuf_.append(tmp, static_cast<size_t>(n));
            metaBytesLeft_ -= static_cast<uint32_t>(n);
            if (!metaBytesLeft_) {
                if (metaCb_) metaCb_(std::string_view{metaBuf_.data(), metaBuf_.size()});
                metaBuf_.clear();
                metaCountdown_ = metaInterval_;
            }
            continue;
        }
        if (metaInterval_ && !metaCountdown_) {
            char lenByte;
            const ssize_t n = pullRaw(std::span{&lenByte, 1}, timeout);
            if (n < 0) {
                outcome = (n == -2) ? ReadResult::AtEof : ReadResult::Closed;
                if (produced) break;
                return {outcome};
            }
            if (n == 0) break;
            metaBytesLeft_ = static_cast<uint32_t>(static_cast<unsigned char>(lenByte)) * 16;
            metaBuf_.clear();
            if (!metaBytesLeft_) metaCountdown_ = metaInterval_;  // empty block, restart
            continue;
        }
        size_t want = buffer.size() - produced;
        if (metaCountdown_) want = std::min<size_t>(want, metaCountdown_);
        const ssize_t n = pullRaw(std::span{buffer}.subspan(produced, want), timeout);
        if (n < 0) {
            outcome = (n == -2) ? ReadResult::AtEof : ReadResult::Closed;
            if (produced) break;
            return {outcome};
        }
        if (n == 0) break;
        if (metaCountdown_) metaCountdown_ -= static_cast<uint32_t>(n);
        produced += static_cast<size_t>(n);
    }
    if (produced) return {ReadResult::Data, produced};
    return {outcome};
}

}  // namespace squeeze2raop2
