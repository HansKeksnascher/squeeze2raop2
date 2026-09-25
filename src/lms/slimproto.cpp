#include "lms/slimproto.h"

#include "common/byte_order.h"
#include "common/log.h"
#include "common/net_util.h"
#include "common/util.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>

// Transport half of the slimproto client: connection lifecycle, discovery,
// the packet reader loop and the raw send path. Packet/opcode encoding and
// decoding live in slimproto_packets.cpp.

namespace squeeze2raop2 {

namespace {

// Reads exactly `len` bytes, retrying EINTR and short reads. Returns false on
// EOF or a real error.
bool recvFully(int fd, void* dst, size_t len) {
    auto* p = static_cast<std::byte*>(dst);
    while (len > 0) {
        const ssize_t n = ::recv(fd, p, len, 0);
        if (n > 0) {
            p += n;
            len -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

}  // namespace

bool discoverLms(std::string& hostOut, uint16_t port, uint32_t timeoutMs) {
    UniqueFd fd{::socket(AF_INET, SOCK_DGRAM, 0)};
    if (!fd) {
        log::error(log::Area::Lms, "discovery socket failed: {}", errnoMessage(errno));
        return false;
    }
    int one = 1;
    (void)setsockopt(fd.get(), SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_port = htons(port);
    d.sin_addr.s_addr = INADDR_BROADCAST;

    const uint64_t deadline = nowMs() + timeoutMs;
    sockaddr_in from{};
    while (nowMs() < deadline) {
        if (sendto(fd.get(), "e", 1, 0, reinterpret_cast<sockaddr*>(&d), sizeof(d)) < 0)
            log::warn(log::Area::Lms, "discovery send failed: {}", errnoMessage(errno));

        const uint64_t now = nowMs();
        if (now >= deadline) break;
        const uint64_t remaining = deadline - now;
        pollfd pfd{fd.get(), POLLIN, 0};
        const int wait = static_cast<int>(std::min<uint64_t>(kDiscoveryPollWaitMs, remaining));
        const int pr = ::poll(&pfd, 1, wait);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        char buf[64];
        socklen_t slen = sizeof(from);
        const ssize_t n =
            recvfrom(fd.get(), buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &slen);
        if (n > 0) {
            buf[static_cast<size_t>(n)] = '\0';
            if (buf[0] == 'E' || buf[0] == 'D') {
                hostOut = ipv4ToString(from.sin_addr);
                log::info(log::Area::Lms, "discovered LMS at {}:{}", hostOut, port);
                return true;
            }
        }
    }
    return false;
}

SlimProtoClient::SlimProtoClient(std::array<uint8_t, 6> mac, std::string caps, Events events)
    : mac_(mac), caps_(std::move(caps)), events_(std::move(events)) {}

SlimProtoClient::~SlimProtoClient() { stop(); }

void SlimProtoClient::setPlayerName(const std::string& name) { playerName_ = name; }

void SlimProtoClient::start(const std::string& host, uint16_t port) {
    host_ = host;
    port_ = port;
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

std::shared_ptr<UniqueFd> SlimProtoClient::currentSock() const {
    return sock_.load(std::memory_order_acquire);
}

void SlimProtoClient::stop() {
    if (thread_.joinable() && !thread_.get_stop_token().stop_requested()) {
        thread_.request_stop();
        // Graceful goodbye: LMS 9.1's BYE! handler is a near no-op (it only
        // reacts to the old SDK upgrade reason), but it is correct protocol
        // and makes the intent visible in LMS logs before the socket drops.
        const uint8_t bye = kByeReasonNormal;
        (void)sendPacket(kOpBye, std::as_bytes(std::span{&bye, 1}));  // best effort
        // shutdown() (not close) wakes a reader blocked in poll()/recv() and is
        // safe concurrently with the best-effort BYE! send above.
        if (auto sock = currentSock()) sock->shutdown();
    }
    if (thread_.joinable()) thread_.join();
    // Drop our reference; a send still in flight closes the fd when it finishes.
    sock_.store(nullptr, std::memory_order_release);
}

bool SlimProtoClient::sendRaw(std::span<const std::byte> data) {
    // Copy the socket reference (atomic snapshot), then do the (possibly
    // blocking) send outside any lock. Holding the shared_ptr, not a lock,
    // keeps the fd alive for the whole send even if a reconnect replaces sock_.
    std::shared_ptr<UniqueFd> sock = currentSock();
    if (!sock || sock->get() < 0) return false;
    std::lock_guard<std::mutex> lock(sendMutex_);
    return sendAll(sock->get(), data.data(), data.size());
}

bool SlimProtoClient::sendPacket(const char (&opcode)[5], std::span<const std::byte> payload) {
    // client -> LMS framing (per squeezelite/HELO spec):
    // [4b opcode][4b big-endian length = payload bytes][payload]
    std::array<std::byte, kFrameHeaderBytes> header{};
    std::memcpy(header.data(), opcode, kOpcodeBytes);
    writeInt<Endian::Big>(header.data() + kLengthFieldOffset,
                          static_cast<uint32_t>(payload.size()));
    std::vector<std::byte> pkt;
    pkt.reserve(kFrameHeaderBytes + payload.size());
    pkt.insert(pkt.end(), header.begin(), header.end());
    if (!payload.empty()) pkt.insert(pkt.end(), payload.begin(), payload.end());
    return sendRaw(std::span{pkt});
}

bool SlimProtoClient::connectOnce(bool reconnect) {
    std::string error;
    const int fd = connectTcp(host_, port_, error);
    if (fd < 0) {
        log::warn(log::Area::Lms, "connect to {}:{} failed: {}", host_, port_, error);
        return false;
    }
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Server-dead resilience (squeezelite's 35 s watchdog parity): kernel
    // keepalive detects a half-open control connection (~2.5 min to fail
    // with these settings) so run() reconnects instead of blocking forever.
    int kaIdle = kKeepAliveIdleSec, kaIntvl = kKeepAliveIntervalSec, kaCnt = kKeepAliveCount;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &kaIdle, sizeof(kaIdle));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &kaIntvl, sizeof(kaIntvl));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &kaCnt, sizeof(kaCnt));

    // Replace (do not reset) the old socket: the previous connection closes
    // once every thread that still holds its shared_ptr drops it.
    sock_.store(std::make_shared<UniqueFd>(fd), std::memory_order_release);
    sendHelo(reconnect);
    return true;
}

void SlimProtoClient::maybeHeartbeat() {
    const uint64_t now = nowMs();
    if (now - lastHeartbeatMs_ >= kHeartbeatIntervalMs) {
        lastHeartbeatMs_ = now;
        sendStat(kStatHeartbeat, statsProvider_ ? statsProvider_() : StreamStats{});
    }
}

void SlimProtoClient::run(std::stop_token st) {
    unsigned fails = 0;
    while (!st.stop_requested()) {
        if (host_.empty() && !discoverLms(host_, port_, kDiscoveryTimeoutMs)) {
            log::warn(log::Area::Lms, "LMS discovery failed, retrying in 5s");
            for (unsigned i = 0;
                 i < static_cast<unsigned>(kDiscoveryRetryIterations) && !st.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryTickMs));
            continue;
        }
        if (!connectOnce(reconnect_)) {
            ++fails;
            unsigned delay =
                std::min<unsigned>(fails * static_cast<unsigned>(kReconnectBackoffFactorSec),
                                   static_cast<unsigned>(kReconnectBackoffCapSec));
            log::warn(log::Area::Lms, "connect to {} failed, retrying in {}s", host_, delay);
            const unsigned ticks = delay * 1000u / kRetryTickMs;
            for (unsigned i = 0; i < ticks && !st.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryTickMs));
            continue;
        }
        fails = 0;
        reconnect_ = true;
        lastServerMsgMs_ = nowMs();

        // Hold this connection's socket for the duration of the read loop so a
        // reconnect cannot close the fd under us; the local reference is
        // released when the loop exits.
        std::shared_ptr<UniqueFd> sock = currentSock();
        if (!sock) break;
        const int fd = sock->get();

        std::string buf;
        size_t expect = 0;
        std::array<char, kReadDrainBufferBytes> tmp{};

        while (!st.stop_requested()) {
            pollfd pfd{fd, POLLIN, 0};
            int pr = ::poll(&pfd, 1, kPollTimeoutMs);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) {
                maybeHeartbeat();
                // Server-silence watchdog (squeezelite parity): no packet for
                // the configured window means the control connection is dead.
                if (serverTimeoutMs_ && lastServerMsgMs_ &&
                    nowMs() - lastServerMsgMs_ > serverTimeoutMs_) {
                    log::warn(log::Area::Lms, "no server messages for {} ms; reconnecting",
                              nowMs() - lastServerMsgMs_);
                    break;
                }
                continue;
            }
            if (expect == 0) {
                std::byte hdr[kLengthPrefixBytes];
                if (!recvFully(fd, hdr, sizeof(hdr))) break;
                expect = static_cast<size_t>(readInt<Endian::Big, uint16_t>(hdr));
                if (expect > kMaxPacketBytes || expect < kMinPacketBytes) {
                    log::error(log::Area::Lms, "bogus packet length {}", expect);
                    break;
                }
                buf.clear();
                buf.reserve(expect);
            } else {
                const size_t want = std::min(tmp.size(), expect - buf.size());
                const ssize_t n = ::recv(fd, tmp.data(), want, 0);
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                    break;
                }
                buf.append(tmp.data(), static_cast<size_t>(n));
                if (buf.size() == expect) {
                    std::string pkt;
                    pkt.swap(buf);
                    expect = 0;
                    process(pkt);
                    lastServerMsgMs_ = nowMs();
                    // heartbeat driven by poll timeout
                }
            }
        }
        log::info(log::Area::Lms, "connection lost");
        if (st.stop_requested()) break;
    }
}

}  // namespace squeeze2raop2
