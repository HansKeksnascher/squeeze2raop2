#include "lms/lms_stream.h"

#include "common/log.h"
#include "common/transport.h"
#include "common/util.h"
#include "lms/icy_meta.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>

namespace squeeze2raop2 {

HttpStreamReader::~HttpStreamReader() { close(); }

void HttpStreamReader::interrupt() {
    // Snapshot under the lock so a concurrent openBlocking()/close() replacing
    // transport_ cannot free it under us; the shared_ptr keeps it (and its
    // descriptor) alive for the duration of this call.
    std::shared_ptr<Transport> transport;
    {
        std::lock_guard<std::mutex> lock(transportMutex_);
        transport = transport_;
    }
    if (transport) transport->interrupt();
}

void HttpStreamReader::resetLocked() {
    std::shared_ptr<Transport> transport;
    {
        std::lock_guard<std::mutex> lock(transportMutex_);
        transport = std::move(transport_);
    }
    if (transport) transport->close();
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

// The host (without port) from a request's Host header, for TLS SNI and
// certificate verification, and for naming the source in logs. Empty when the
// request has no Host header. LMS builds it from the URL it handed over, so on
// a direct stream it is the station's hostname.
std::string requestHost(std::string_view request) {
    auto iequals = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (tolower(static_cast<unsigned char>(a[i])) !=
                tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    size_t pos = 0;
    while (pos < request.size()) {
        const size_t eol = request.find('\n', pos);
        std::string_view line(request.data() + pos,
                              (eol == std::string::npos ? request.size() : eol) - pos);
        pos = (eol == std::string::npos) ? request.size() : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        if (line.size() <= 5 || !iequals(line.substr(0, 5), "host:")) continue;
        std::string_view host = line.substr(5);
        while (!host.empty() && (host.front() == ' ' || host.front() == '\t'))
            host.remove_prefix(1);
        // Strip a trailing :port; IPv6 literals arrive bracketed.
        if (!host.empty() && host.front() == '[') {
            const size_t close = host.find(']');
            if (close != std::string_view::npos) host = host.substr(1, close - 1);
        } else {
            const size_t colon = host.rfind(':');
            if (colon != std::string_view::npos && colon + 1 < host.size()) {
                bool digits = true;
                for (size_t i = colon + 1; i < host.size(); ++i)
                    if (!std::isdigit(static_cast<unsigned char>(host[i]))) digits = false;
                if (digits) host = host.substr(0, colon);
            }
        }
        return std::string(host);
    }
    return std::string();
}

bool HttpStreamReader::openBlocking(const std::string& host, uint16_t port,
                                    const std::string& request, std::string& errorOut, bool ssl) {
    // Serialize against a concurrent close() (PlayerSession::stop() racing a
    // strm s). Held for the whole header phase; interrupt() still works because
    // it does not need this lock.
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    resetLocked();

    const std::string sniHost = ssl ? requestHost(request) : std::string();
    std::shared_ptr<Transport> transport = makeTransport(ssl, sniHost);
    if (!transport) {
        errorOut = "HTTPS not supported in this build";
        return false;
    }
    if (ssl)
        log::info(log::Area::Lms, "TLS connect {}:{} (sni={})", host, port,
                  sniHost.empty() ? "?" : sniHost);
    if (!transport->connect(host, port, errorOut)) return false;
    {
        std::lock_guard<std::mutex> lock(transportMutex_);
        transport_ = transport;
    }

    std::string wire = request;
    if (wire.find("\r\n\r\n") == std::string::npos) {
        if (wire.size() < 4 || wire.compare(wire.size() - 2, 2, "\r\n") != 0) wire += "\r\n";
        wire += "\r\n";
    }
    std::string sendError;
    if (!transport->writeAll(std::span{wire.data(), wire.size()}, sendError)) {
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
        const auto wait = static_cast<uint32_t>(std::min<uint64_t>(1000, deadline - now));
        char chunk[4096];
        const Transport::Read r = transport->read(std::span{chunk, sizeof(chunk)}, wait);
        switch (r.result) {
        case Transport::Result::Data: break;
        case Transport::Result::Timeout: continue;
        case Transport::Result::Eof:
            errorOut = "closed while reading headers";
            resetLocked();
            return false;
        case Transport::Result::Error:
            errorOut = "header read: " + transport->lastError();
            resetLocked();
            return false;
        }
        buf.append(chunk, r.bytes);
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
        if (metaInterval_)
            log::info(log::Area::Lms, "icy metadata active, interval={}", metaInterval_);
        return true;
    }
}

// Pull raw bytes from leftover_/transport: >0 = bytes, 0 = no data yet
// (timeout), -1 = socket error, -2 = orderly EOF.
ssize_t HttpStreamReader::pullRaw(std::span<char> dst, uint32_t timeoutMs) {
    if (!leftover_.empty()) {
        size_t n = std::min(leftover_.size(), dst.size());
        leftover_.copy(dst.data(), n);
        leftover_.erase(0, n);
        return static_cast<ssize_t>(n);
    }
    // Snapshot under the lock; the shared_ptr keeps the transport alive across
    // the blocking read even if close()/a new open replaces it meanwhile.
    std::shared_ptr<Transport> transport;
    {
        std::lock_guard<std::mutex> lock(transportMutex_);
        transport = transport_;
    }
    if (!transport) return -1;
    const Transport::Read r = transport->read(dst, timeoutMs);
    switch (r.result) {
    case Transport::Result::Data: return static_cast<ssize_t>(r.bytes);
    case Transport::Result::Timeout: return 0;
    case Transport::Result::Eof: return -2;
    case Transport::Result::Error: return -1;
    }
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
                break;
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
                break;
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
            break;
        }
        if (n == 0) break;
        if (metaCountdown_) metaCountdown_ -= static_cast<uint32_t>(n);
        produced += static_cast<size_t>(n);
    }
    if (produced) return {ReadResult::Data, produced};
    return {outcome};
}

}  // namespace squeeze2raop2