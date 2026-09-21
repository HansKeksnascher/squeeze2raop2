#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sq2 {

class HttpStreamReader {
public:
    enum class ReadResult { Data, Closed, AtEof };

    HttpStreamReader() = default;
    ~HttpStreamReader();
    HttpStreamReader(const HttpStreamReader&) = delete;
    HttpStreamReader& operator=(const HttpStreamReader&) = delete;

    bool openBlocking(const std::string& host, uint16_t port, const std::string& request,
                      std::string& errorOut);

    ReadResult read(char* buffer, size_t maxLen, size_t* gotOut, uint32_t timeoutMs);

    void close();

    const std::string& headers() const { return headers_; };

    // ICY in-band metadata (squeezelite parity): when the response carried
    // icy-metaint, read() strips the interleaved metadata blocks from the
    // audio and delivers each complete block here.
    void setMetaCallback(std::function<void(const char*, size_t)> cb) { metaCb_ = std::move(cb); }
    uint32_t metaInterval() const { return metaInterval_; }

private:
    ssize_t pullRaw(char* dst, size_t max, uint32_t timeoutMs);

    int fd_ = -1;
    std::string headers_;
    std::string leftover_;
    bool headersDone_ = false;

    uint32_t metaInterval_ = 0;   // icy-metaint; 0 = no icy metadata
    uint32_t metaCountdown_ = 0;  // audio bytes until the next length byte
    uint32_t metaBytesLeft_ = 0;  // metadata payload bytes still pending
    std::string metaBuf_;
    std::function<void(const char*, size_t)> metaCb_;
};

}
