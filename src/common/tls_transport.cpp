#include "common/tls_transport.h"

#include "common/log.h"
#include "common/net_util.h"
#include "common/third_party_warnings.h"
#include "common/util.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <utility>

// Vendored mbedTLS headers are not ours to fix; keep the project's full
// warning set for this file's own code.
SQUEEZE2RAOP2_TP_WARNINGS_PUSH
#pragma GCC diagnostic ignored "-Wpedantic"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
SQUEEZE2RAOP2_TP_WARNINGS_POP

namespace squeeze2raop2 {

namespace {

constexpr uint64_t kIoTimeoutMs = 10000;

std::string tlsErrorText(int rc) {
    char buf[256] = {0};
    mbedtls_strerror(rc, buf, sizeof(buf) - 1);
    return buf[0] ? std::string(buf) : ("error " + std::to_string(rc));
}

// Process-wide trust context: one ssl config + CA chain + RNG shared by every
// connection. Initialized once (first caller wins), after which the ssl
// contexts copy nothing and merely reference this config.
struct GlobalTls {
    std::once_flag once;
    bool ok = false;
    std::string error;
    bool verify = true;
    mbedtls_ssl_config conf{};
    mbedtls_x509_crt cacert{};
    mbedtls_ctr_drbg_context drbg{};
    mbedtls_entropy_context entropy{};
};

GlobalTls& gTls() {
    static GlobalTls g;
    return g;
}

// Parse a CA bundle file into `g.cacert`. rc > 0 counts individual cert parse
// failures (the rest loaded); accept those, fail only on a fatal error.
bool addCrtFile(GlobalTls& g, const std::string& path) {
    const int rc = mbedtls_x509_crt_parse_file(&g.cacert, path.c_str());
    if (rc < 0) return false;
    if (rc > 0) log::warn(log::Area::App, "tls-ca {}: {} certificate(s) skipped", path, rc);
    return true;
}

bool addCrtPath(GlobalTls& g, const std::string& path) {
    // mbedtls requires a trailing slash on a CA directory.
    return mbedtls_x509_crt_parse_path(&g.cacert, path.c_str()) >= 0;
}

bool loadCa(const std::string& explicitPath, std::string& error) {
    GlobalTls& g = gTls();
    if (!explicitPath.empty()) {
        if (addCrtFile(g, explicitPath)) return true;
        error = "cannot load CA bundle " + explicitPath;
        return false;
    }
    if (const char* env = std::getenv("SSL_CERT_FILE"); env && *env && addCrtFile(g, env))
        return true;
    static const char* kFiles[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
    };
    for (const char* p : kFiles)
        if (addCrtFile(g, p)) return true;
    if (const char* env = std::getenv("SSL_CERT_DIR");
        env && *env && addCrtPath(g, std::string(env) + "/"))
        return true;
    static const char* kDirs[] = {"/etc/ssl/certs/", "/etc/pki/tls/certs/"};
    for (const char* d : kDirs)
        if (addCrtPath(g, d)) return true;
    error = "no CA certificates found; set [global] tls-ca or tls-verify = off";
    return false;
}

void initGlobal(const tls::Options& options) {
    GlobalTls& g = gTls();
    mbedtls_ssl_config_init(&g.conf);
    mbedtls_x509_crt_init(&g.cacert);
    mbedtls_ctr_drbg_init(&g.drbg);
    mbedtls_entropy_init(&g.entropy);
    g.verify = options.verify;

    int rc = mbedtls_ssl_config_defaults(&g.conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        g.error = "tls config: " + tlsErrorText(rc);
        return;
    }
    // TLS 1.3 performs its ECDH key share through PSA, which mbedTLS does not
    // initialize for us; without this the first handshake dies with
    // "internal error" from psa_generate_key().
    const psa_status_t psa = psa_crypto_init();
    if (psa != PSA_SUCCESS) {
        g.error = "psa_crypto_init failed: " + std::to_string(static_cast<int>(psa));
        return;
    }
    rc = mbedtls_ctr_drbg_seed(&g.drbg, mbedtls_entropy_func, &g.entropy, nullptr, 0);
    if (rc != 0) {
        g.error = "tls rng seed: " + tlsErrorText(rc);
        return;
    }
    mbedtls_ssl_conf_rng(&g.conf, mbedtls_ctr_drbg_random, &g.drbg);

    if (!options.verify) {
        mbedtls_ssl_conf_authmode(&g.conf, MBEDTLS_SSL_VERIFY_NONE);
        g.ok = true;
        return;
    }
    std::string caError;
    if (!loadCa(options.caPath, caError)) {
        g.error = caError;
        return;
    }
    mbedtls_ssl_conf_ca_chain(&g.conf, &g.cacert, nullptr);
    mbedtls_ssl_conf_authmode(&g.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    g.ok = true;
}

// --- mbedTLS BIO over a raw, non-blocking descriptor. MSG_NOSIGNAL keeps a
// dead peer from killing the process (mbedTLS's own net_sockets uses a plain
// send()), and EAGAIN maps to the WANT_* codes the poll-driven caller retries.
int bioSend(void* ctx, const unsigned char* buf, size_t len) {
    const int fd = *static_cast<int*>(ctx);
    ssize_t n;
    do {
        n = ::send(fd, buf, len, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return static_cast<int>(n);
}

int bioRecv(void* ctx, unsigned char* buf, size_t len) {
    const int fd = *static_cast<int*>(ctx);
    ssize_t n;
    do {
        n = ::recv(fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return static_cast<int>(n);  // 0 = peer closed
}

}  // namespace

struct TlsTransport::Impl {
    UniqueFd fd;
    mutable std::mutex fdMutex;
    std::string error;
    std::string hostname;
    int bioFd = -1;
    mbedtls_ssl_context ssl{};
    bool sslInit = false;
    bool sslSetup = false;
};

TlsTransport::TlsTransport(std::string hostname) : impl_(std::make_unique<Impl>()) {
    impl_->hostname = std::move(hostname);
}

TlsTransport::~TlsTransport() { close(); }

int TlsTransport::fdSnapshot() const {
    std::lock_guard<std::mutex> lock(impl_->fdMutex);
    return impl_->fd.get();
}

void TlsTransport::setError(std::string message) { impl_->error = std::move(message); }

// 1 = ready, 0 = deadline reached, -1 = poll error (lastError() set).
int TlsTransport::waitIo(bool wantRead, uint64_t deadline) {
    const int fd = fdSnapshot();
    if (fd < 0) {
        setError("not connected");
        return -1;
    }
    const short events = wantRead ? POLLIN : POLLOUT;
    for (;;) {
        const uint64_t now = nowMs();
        if (now >= deadline) return 0;
        const int wait = static_cast<int>(std::min<uint64_t>(1000, deadline - now));
        pollfd pfd{fd, events, 0};
        const int pr = ::poll(&pfd, 1, wait);
        if (pr > 0) return 1;
        if (pr < 0 && errno != EINTR) {
            setError(std::string("poll: ") + errnoMessage(errno));
            return -1;
        }
    }
}

bool TlsTransport::handshake(std::string& error) {
    const uint64_t deadline = nowMs() + kIoTimeoutMs;
    for (;;) {
        const int rc = mbedtls_ssl_handshake(&impl_->ssl);
        if (rc == 0) return true;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            const int w = waitIo(rc == MBEDTLS_ERR_SSL_WANT_READ, deadline);
            if (w == 0) {
                error = "tls handshake timed out";
                setError(error);
                return false;
            }
            if (w < 0) {
                error = impl_->error;
                return false;
            }
            continue;
        }
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            const uint32_t flags = mbedtls_ssl_get_verify_result(&impl_->ssl);
            char info[512] = {0};
            mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
            std::string msg = "certificate verification failed";
            if (!impl_->hostname.empty()) msg += " for " + impl_->hostname;
            if (info[0]) msg += ": " + std::string(info);
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
            error = msg;
            setError(msg);
            return false;
        }
        error = "tls handshake: " + tlsErrorText(rc);
        setError(error);
        return false;
    }
}

bool TlsTransport::connect(const std::string& host, uint16_t port, std::string& error) {
    close();
    const int raw = connectTcp(host, port, error);
    if (raw < 0) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->fdMutex);
        impl_->fd.reset(raw);
    }
    // Bounded close: an abandoned connection must not hang close() forever.
    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 3;
    (void)setsockopt(raw, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    enableTcpKeepalive(raw);
    const int fl = ::fcntl(raw, F_GETFL, 0);
    if (fl >= 0) (void)::fcntl(raw, F_SETFL, fl | O_NONBLOCK);
    impl_->bioFd = raw;

    // Idempotent: an explicit tlsConfigure() from startup wins; otherwise this
    // lazily initializes with defaults (verify on, system trust store).
    std::string cfgError;
    if (!tlsConfigure(tls::Options{}, cfgError)) {
        error = cfgError;
        close();
        return false;
    }

    mbedtls_ssl_init(&impl_->ssl);
    impl_->sslInit = true;
    int rc = mbedtls_ssl_setup(&impl_->ssl, &gTls().conf);
    if (rc != 0) {
        error = "tls setup: " + tlsErrorText(rc);
        close();
        return false;
    }
    impl_->sslSetup = true;
    if (!impl_->hostname.empty()) {
        rc = mbedtls_ssl_set_hostname(&impl_->ssl, impl_->hostname.c_str());
        if (rc != 0) {
            error = "tls hostname: " + tlsErrorText(rc);
            close();
            return false;
        }
    }
    mbedtls_ssl_set_bio(&impl_->ssl, &impl_->bioFd, bioSend, bioRecv, nullptr);
    if (!handshake(error)) {
        close();
        return false;
    }
    return true;
}

bool TlsTransport::writeAll(std::span<const char> data, std::string& error) {
    if (!impl_->sslSetup) {
        error = "not connected";
        return false;
    }
    const uint64_t deadline = nowMs() + kIoTimeoutMs;
    size_t off = 0;
    while (off < data.size()) {
        const int n = mbedtls_ssl_write(&impl_->ssl,
                                        reinterpret_cast<const unsigned char*>(data.data() + off),
                                        data.size() - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            const int w = waitIo(n == MBEDTLS_ERR_SSL_WANT_READ, deadline);
            if (w == 0) {
                error = "tls write timed out";
                setError(error);
                return false;
            }
            if (w < 0) {
                error = impl_->error;
                return false;
            }
            continue;
        }
        error = "tls write: " + tlsErrorText(n);
        setError(error);
        return false;
    }
    return true;
}

TlsTransport::Read TlsTransport::read(std::span<char> buffer, uint32_t timeoutMs) {
    if (!impl_->sslSetup) {
        setError("not connected");
        return {Result::Error, 0};
    }
    const uint64_t deadline = nowMs() + timeoutMs;
    for (;;) {
        // ssl_read returns buffered application data without touching the
        // socket; it only yields WANT_* when it genuinely needs more bytes.
        const int n = mbedtls_ssl_read(&impl_->ssl, reinterpret_cast<unsigned char*>(buffer.data()),
                                       buffer.size());
        if (n > 0) return {Result::Data, static_cast<size_t>(n)};
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return {Result::Eof, 0};
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            const int w = waitIo(n == MBEDTLS_ERR_SSL_WANT_READ, deadline);
            if (w == 0) return {Result::Timeout, 0};
            if (w < 0) return {Result::Error, 0};
            continue;
        }
#if defined(MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            if (nowMs() >= deadline) return {Result::Timeout, 0};
            continue;  // no network round-trip needed
        }
#endif
        setError("tls read: " + tlsErrorText(n));
        return {Result::Error, 0};
    }
}

void TlsTransport::interrupt() {
    std::lock_guard<std::mutex> lock(impl_->fdMutex);
    impl_->fd.shutdown();
}

void TlsTransport::close() {
    if (impl_->sslInit) {
        mbedtls_ssl_free(&impl_->ssl);
        impl_->sslInit = false;
        impl_->sslSetup = false;
    }
    std::lock_guard<std::mutex> lock(impl_->fdMutex);
    impl_->fd.reset();
    impl_->bioFd = -1;
}

const std::string& TlsTransport::lastError() const { return impl_->error; }

// --- process-wide setup (declared in transport.h) ---

bool tlsConfigure(const tls::Options& options, std::string& error) {
    GlobalTls& g = gTls();
    std::call_once(g.once, [&options] { initGlobal(options); });
    error = g.error;
    return g.ok;
}

bool tlsUsable() {
    std::string ignored;
    return tlsConfigure(tls::Options{}, ignored);
}

}  // namespace squeeze2raop2