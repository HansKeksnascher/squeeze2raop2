#include "slimproto.h"

#include "log.h"
#include "net_util.h"
#include "util.h"

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

constexpr size_t kMaxPacket = size_t{4096} * 8;
constexpr int kPollTimeoutMs = 100;

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
        log::error("discovery socket failed: {}", errnoMessage(errno));
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
            log::warn("discovery send failed: {}", errnoMessage(errno));

        const uint64_t now = nowMs();
        if (now >= deadline) break;
        const uint64_t remaining = deadline - now;
        pollfd pfd{fd.get(), POLLIN, 0};
        const int wait = static_cast<int>(std::min<uint64_t>(2000, remaining));
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
                log::info("discovered LMS at {}:{}", hostOut, port);
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
    std::lock_guard<std::mutex> lock(sockMutex_);
    return sock_;
}

void SlimProtoClient::stop() {
    if (thread_.joinable() && !thread_.get_stop_token().stop_requested()) {
        thread_.request_stop();
        // Graceful goodbye: LMS 9.1's BYE! handler is a near no-op (it only
        // reacts to the old SDK upgrade reason), but it is correct protocol
        // and makes the intent visible in LMS logs before the socket drops.
        const uint8_t bye = 0;
        (void)sendPacket("BYE!", std::as_bytes(std::span{&bye, 1}));  // best effort
        // shutdown() (not close) wakes a reader blocked in poll()/recv() and is
        // safe concurrently with the best-effort BYE! send above.
        if (auto sock = currentSock()) sock->shutdown();
    }
    if (thread_.joinable()) thread_.join();
    // Drop our reference; a send still in flight closes the fd when it finishes.
    std::lock_guard<std::mutex> lock(sockMutex_);
    sock_.reset();
}

bool SlimProtoClient::sendRaw(std::span<const std::byte> data) {
    // Copy the socket reference under sockMutex_, then release the lock before
    // the (possibly blocking) send. Holding the shared_ptr, not the lock, keeps
    // the fd alive for the whole send even if a reconnect replaces sock_.
    std::shared_ptr<UniqueFd> sock = currentSock();
    if (!sock || sock->get() < 0) return false;
    std::lock_guard<std::mutex> lock(sendMutex_);
    return sendAll(sock->get(), data.data(), data.size());
}

bool SlimProtoClient::sendPacket(const char (&opcode)[5], std::span<const std::byte> payload) {
    // client -> LMS framing (per squeezelite/HELO spec):
    // [4b opcode][4b big-endian length = payload bytes][payload]
    std::array<std::byte, 8> header{};
    std::memcpy(header.data(), opcode, 4);
    packN(std::span{header}.subspan(4, 4), payload.size(), 4);
    std::vector<std::byte> pkt;
    pkt.reserve(8 + payload.size());
    pkt.insert(pkt.end(), header.begin(), header.end());
    if (!payload.empty()) pkt.insert(pkt.end(), payload.begin(), payload.end());
    return sendRaw(std::span{pkt});
}

bool SlimProtoClient::connectOnce(bool reconnect) {
    std::string error;
    const int fd = connectTcp(host_, port_, error);
    if (fd < 0) {
        log::warn("connect to {}:{} failed: {}", host_, port_, error);
        return false;
    }
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Server-dead resilience (squeezelite's 35 s watchdog parity): kernel
    // keepalive detects a half-open control connection (~2.5 min to fail
    // with these settings) so run() reconnects instead of blocking forever.
    int kaIdle = 30, kaIntvl = 10, kaCnt = 6;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &kaIdle, sizeof(kaIdle));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &kaIntvl, sizeof(kaIntvl));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &kaCnt, sizeof(kaCnt));

    // Replace (do not reset) the old socket: the previous connection closes
    // once every thread that still holds its shared_ptr drops it.
    {
        std::lock_guard<std::mutex> lock(sockMutex_);
        sock_ = std::make_shared<UniqueFd>(fd);
    }
    sendHelo(reconnect);
    return true;
}

void SlimProtoClient::maybeHeartbeat() {
    const uint64_t now = nowMs();
    if (now - lastHeartbeatMs_ >= 1000) {
        lastHeartbeatMs_ = now;
        sendStat("STMt", statsProvider_ ? statsProvider_() : StreamStats{});
    }
}

void SlimProtoClient::run(std::stop_token st) {
    unsigned fails = 0;
    while (!st.stop_requested()) {
        if (host_.empty() && !discoverLms(host_, port_, 5000)) {
            log::warn("LMS discovery failed, retrying in 5s");
            for (unsigned i = 0; i < 50 && !st.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!connectOnce(reconnect_)) {
            ++fails;
            unsigned delay = std::min<unsigned>(fails * 2, 15);
            log::warn("connect to {} failed, retrying in {}s", host_, delay);
            for (unsigned i = 0; i < delay * 10 && !st.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        fails = 0;
        reconnect_ = true;

        // Hold this connection's socket for the duration of the read loop so a
        // reconnect cannot close the fd under us; the local reference is
        // released when the loop exits.
        std::shared_ptr<UniqueFd> sock = currentSock();
        if (!sock) break;
        const int fd = sock->get();

        std::string buf;
        size_t expect = 0;
        std::array<char, 2048> tmp{};

        while (!st.stop_requested()) {
            pollfd pfd{fd, POLLIN, 0};
            int pr = ::poll(&pfd, 1, kPollTimeoutMs);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) {
                maybeHeartbeat();
                continue;
            }
            if (expect == 0) {
                uint8_t hdr[2];
                if (!recvFully(fd, hdr, sizeof(hdr))) break;
                expect = static_cast<size_t>((hdr[0] << 8) | hdr[1]);
                if (expect > kMaxPacket || expect < 4) {
                    log::error("bogus packet length {}", expect);
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
                    // heartbeat driven by poll timeout
                }
            }
        }
        log::info("connection lost");
        if (st.stop_requested()) break;
    }
}

}  // namespace squeeze2raop2
