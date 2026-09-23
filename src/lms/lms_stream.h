#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include "common/net_util.h"  // UniqueFd

namespace squeeze2raop2 {

class HttpStreamReader {
public:
    // Timeout: no data within the deadline (keep going). Closed: socket
    // error (the stream is dead). AtEof: orderly end of stream.
    enum class ReadResult : std::uint8_t { Data, Timeout, Closed, AtEof };

    HttpStreamReader() = default;
    ~HttpStreamReader();
    HttpStreamReader(const HttpStreamReader&) = delete;
    HttpStreamReader& operator=(const HttpStreamReader&) = delete;

    bool openBlocking(const std::string& host, uint16_t port, const std::string& request,
                      std::string& errorOut);

    // Outcome of one read() call: result == Data carries `bytes` audio bytes
    // in the buffer (a close/error after partial data is reported as Data
    // first, with the close surfacing on the next call). Timeout leaves
    // `bytes` at 0, Closed/AtEof end the stream.
    struct StreamRead {
        ReadResult result;
        size_t bytes = 0;
    };

    StreamRead read(std::span<char> buffer, uint32_t timeoutMs);

    // Wake a reader blocked in read(): SHUT_RDWR on the socket without closing
    // it. Safe to call from another thread while read() runs; the descriptor
    // stays valid until close(). Intended for PlayerSession::stopPlayback(),
    // which interrupts before joining the stream thread.
    void interrupt();

    void close();

    const std::string& headers() const { return headers_; }

    // ICY in-band metadata (squeezelite parity): when the response carried
    // icy-metaint, read() strips the interleaved metadata blocks from the
    // audio and delivers each complete block here.
    void setMetaCallback(std::function<void(std::string_view)> cb) { metaCb_ = std::move(cb); }

private:
    // >0 = bytes, 0 = no data yet (timeout), -1 = socket error, -2 = orderly EOF
    ssize_t pullRaw(std::span<char> dst, uint32_t timeoutMs);

    // Current descriptor under fdMutex_, or -1. The descriptor is only ever
    // closed by close(); interrupt() merely shuts it down, so a value read
    // here stays valid for the duration of one pullRaw()/read() call.
    [[nodiscard]] int fd() const;

    // Clear descriptor + header/ICY state. Caller must hold lifecycleMutex_.
    void resetLocked();

    UniqueFd fd_;
    mutable std::mutex fdMutex_;
    // Serializes openBlocking() against close() (they run on different threads
    // when PlayerSession::stop() races a strm s); interrupt() deliberately does
    // not take it, so it can unblock a header read still in progress.
    std::mutex lifecycleMutex_;
    std::string headers_;
    std::string leftover_;

    uint32_t metaInterval_ = 0;   // icy-metaint; 0 = no icy metadata
    uint32_t metaCountdown_ = 0;  // audio bytes until the next length byte
    uint32_t metaBytesLeft_ = 0;  // metadata payload bytes still pending
    std::string metaBuf_;
    std::function<void(std::string_view)> metaCb_;
};

}  // namespace squeeze2raop2
