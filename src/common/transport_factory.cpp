#include "common/transport.h"

#include "common/tcp_transport.h"
#if defined(SQUEEZE2RAOP2_WITH_HTTPS)
#include "common/tls_transport.h"
#endif

#include <memory>

namespace squeeze2raop2 {

#if !defined(SQUEEZE2RAOP2_WITH_HTTPS)
// No TLS in this build: setup is a no-op and HTTPS streams are unsupported.
bool tlsConfigure(const tls::Options&, std::string&) { return true; }
bool tlsUsable() { return false; }
#endif

std::shared_ptr<Transport> makeTransport(bool ssl, const std::string& sniHostname) {
#if defined(SQUEEZE2RAOP2_WITH_HTTPS)
    if (ssl) return std::make_shared<TlsTransport>(sniHostname);
#else
    (void)sniHostname;
    if (ssl) return nullptr;
#endif
    return std::make_shared<TcpTransport>();
}

}  // namespace squeeze2raop2