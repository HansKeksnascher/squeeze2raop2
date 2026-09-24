#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace squeeze2raop2 {

// A connected byte stream with timeout-aware reads and cross-thread interrupt.
// The plain-TCP implementation (TcpTransport) and, in a TLS build, the
// TLS-over-TCP one (TlsTransport) both sit behind this, so the HTTP/ICY layer
// above never sees sockets or mbedTLS.
//
// Lifecycle contract:
//  - connect() / writeAll() / read() run on the owner's thread only.
//  - interrupt() may be called from another thread while read() is parked; it
//    must make it return promptly and must NOT free the connection (only shut
//    the descriptor down), so a transport kept alive by shared ownership stays
//    valid for the duration of one read().
//  - close() is not concurrent-safe against read(): the owner must ensure the
//    reader has stopped (its thread joined) before closing. An implementation
//    frees its per-connection state (socket, TLS contexts) here.
class Transport {
public:
    // Data: `bytes` > 0; Timeout: nothing within the deadline; Eof: orderly end
    // of stream; Error: socket/TLS failure (lastError() describes it).
    enum class Result : std::uint8_t { Data, Timeout, Eof, Error };
    struct Read {
        Result result = Result::Error;
        size_t bytes = 0;
    };

    virtual ~Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Resolve + connect; false with `error` set on failure.
    virtual bool connect(const std::string& host, uint16_t port, std::string& error) = 0;
    // Write all bytes; false with `error` set on a real failure.
    virtual bool writeAll(std::span<const char> data, std::string& error) = 0;
    virtual Read read(std::span<char> buffer, uint32_t timeoutMs) = 0;
    // Concurrency-safe: unblocks a read() parked in a syscall.
    virtual void interrupt() = 0;
    // Owner-thread only; idempotent.
    virtual void close() = 0;
    // Detail for the most recent Result::Error, empty otherwise.
    [[nodiscard]] virtual const std::string& lastError() const = 0;

protected:
    Transport() = default;
};

namespace tls {

// Process-wide TLS trust settings ([global] tls-verify / tls-ca).
struct Options {
    bool verify = true;
    // Explicit CA bundle/path; empty = autodetect the system trust store.
    std::string caPath;
};

}  // namespace tls

// Configure the process-wide TLS trust context from [global]. Idempotent: the
// first call wins. Returns false with `error` when TLS is requested but its
// context cannot be initialized (e.g. verify is on and no CA was found).
// A no-op returning true in a build without TLS.
bool tlsConfigure(const tls::Options& options, std::string& error);

// True when this build has TLS *and* its trust context initialized OK, i.e.
// direct HTTPS streams can actually be established. Used to decide whether to
// advertise CanHTTPS=1 to LMS; false makes LMS keep proxying.
bool tlsUsable();

// Build the byte stream for a track: a TLS-over-TCP transport when `ssl`,
// otherwise plain TCP. `sniHostname` (the request's Host, without port) is the
// SNI / certificate-verification name. Returns nullptr when `ssl` is requested
// but this build has no TLS.
std::shared_ptr<Transport> makeTransport(bool ssl, const std::string& sniHostname);

}  // namespace squeeze2raop2