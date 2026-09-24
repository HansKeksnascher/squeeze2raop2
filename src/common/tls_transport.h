#pragma once

#include "common/transport.h"

#include <memory>
#include <string>

namespace squeeze2raop2 {

// TLS-over-TCP implementation of Transport, backed by the vendored mbedTLS.
// The mbedTLS types are hidden behind Impl so this header stays dependency
// free. Certificate trust comes from the process-wide context set up by
// tlsConfigure(); `hostname` is used for SNI and certificate verification.
//
// The descriptor is non-blocking: the handshake and every read are driven by
// poll() with a deadline, which keeps them both bounded and interruptible via
// interrupt() (fd shutdown) from another thread.
class TlsTransport final : public Transport {
public:
    explicit TlsTransport(std::string hostname);
    ~TlsTransport() override;

    bool connect(const std::string& host, uint16_t port, std::string& error) override;
    bool writeAll(std::span<const char> data, std::string& error) override;
    Read read(std::span<char> buffer, uint32_t timeoutMs) override;
    void interrupt() override;
    void close() override;
    [[nodiscard]] const std::string& lastError() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    [[nodiscard]] int fdSnapshot() const;
    // 1 = ready, 0 = deadline reached, -1 = poll error (lastError() set).
    int waitIo(bool wantRead, uint64_t deadline);
    bool handshake(std::string& error);
    void setError(std::string message);
};

}  // namespace squeeze2raop2