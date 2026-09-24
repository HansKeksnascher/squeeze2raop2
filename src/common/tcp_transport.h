#pragma once

#include "common/net_util.h"  // UniqueFd
#include "common/transport.h"

#include <mutex>
#include <string>

namespace squeeze2raop2 {

// Plain-TCP implementation of Transport: the byte stream under an HTTP
// request. Owns the connected descriptor; interrupt() shuts it down (so a
// concurrent read() unblocks) while close() — owner-thread only — actually
// closes it.
class TcpTransport final : public Transport {
public:
    TcpTransport() = default;
    ~TcpTransport() override;

    bool connect(const std::string& host, uint16_t port, std::string& error) override;
    bool writeAll(std::span<const char> data, std::string& error) override;
    Read read(std::span<char> buffer, uint32_t timeoutMs) override;
    void interrupt() override;
    void close() override;
    [[nodiscard]] const std::string& lastError() const override { return error_; }

private:
    // Current descriptor under fdMutex_, or -1. Only close() ever closes it;
    // interrupt() merely shuts it down, so a value read here stays valid for
    // the duration of one read()/writeAll() call.
    [[nodiscard]] int fd() const;
    void setError(std::string message);

    UniqueFd fd_;
    mutable std::mutex fdMutex_;
    std::string error_;
};

}  // namespace squeeze2raop2