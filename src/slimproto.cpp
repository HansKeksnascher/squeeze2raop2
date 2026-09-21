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
#include <chrono>
#include <cstring>
#include <span>
#include <thread>

// Transport half of the slimproto client: connection lifecycle, discovery,
// the packet reader loop and the raw send path. Packet/opcode encoding and
// decoding live in slimproto_packets.cpp.

namespace squeeze2raop2 {

namespace {

constexpr size_t kMaxPacket = size_t{4096} * 8;
constexpr int kPollTimeoutMs = 100;

} // namespace

bool discoverLms(std::string& hostOut, uint16_t port, uint32_t timeoutMs) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log::error("discovery socket failed: {}", errnoMessage(errno));
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_port = htons(port);
    d.sin_addr.s_addr = INADDR_BROADCAST;

    uint64_t deadline = nowMs() + timeoutMs;
    sockaddr_in from{};
    while (nowMs() < deadline) {
        if (sendto(fd, "e", 1, 0, reinterpret_cast<sockaddr*>(&d), sizeof(d)) < 0)
            log::warn("discovery send failed: {}", errnoMessage(errno));

        pollfd pfd{fd, POLLIN, 0};
        uint32_t wait = std::min<uint32_t>(2000, static_cast<uint32_t>(deadline - nowMs()));
        if (poll(&pfd, 1, static_cast<int>(wait)) == 1) {
            char buf[64];
            socklen_t slen = sizeof(from);
            ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                                 reinterpret_cast<sockaddr*>(&from), &slen);
            if (n > 0) {
                buf[static_cast<size_t>(n)] = '\0';
                if (buf[0] == 'E' || buf[0] == 'D') {
                    ::close(fd);
                    hostOut = ipv4ToString(from.sin_addr);
                    log::info("discovered LMS at {}:{}", hostOut, port);
                    return true;
                }
            }
        }
    }
    ::close(fd);
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

void SlimProtoClient::stop() {
    if (thread_.joinable() && !thread_.get_stop_token().stop_requested()) {
        thread_.request_stop();
        // Graceful goodbye: LMS 9.1's BYE! handler is a near no-op (it only
        // reacts to the old SDK upgrade reason), but it is correct protocol
        // and makes the intent visible in LMS logs before the socket drops.
        const uint8_t bye = 0;
        sendPacket("BYE!", std::as_bytes(std::span{&bye, 1}));
        if (sock_ >= 0) {
            ::shutdown(sock_, SHUT_RDWR);
        }
    }
    if (thread_.joinable()) thread_.join();
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

bool SlimProtoClient::sendRaw(std::span<const std::byte> data) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    return sendAll(sock_, data.data(), data.size());
}

bool SlimProtoClient::sendPacket(const char (&opcode)[5], std::span<const std::byte> payload) {
    if (sock_ < 0) return false;
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
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
    std::string error;
    sock_ = connectTcp(host_, port_, error);
    if (sock_ < 0) return false;
    int one = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Server-dead resilience (squeezelite's 35 s watchdog parity): kernel
    // keepalive detects a half-open control connection (~2.5 min to fail
    // with these settings) so run() reconnects instead of blocking forever.
    int kaIdle = 30, kaIntvl = 10, kaCnt = 6;
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPIDLE, &kaIdle, sizeof(kaIdle));
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPINTVL, &kaIntvl, sizeof(kaIntvl));
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPCNT, &kaCnt, sizeof(kaCnt));
    sendHelo(reconnect);
    return true;
}

void SlimProtoClient::maybeHeartbeat() {
    uint64_t now = nowMs();
    if (now - lastHeartbeatMs_ >= 1000) {
        lastHeartbeatMs_ = now;
        if (statsProvider_) sendStat("STMt", statsProvider_());
        else sendStat("STMt", {});
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

        std::string buf;
        size_t expect = 0;
        char tmp[2048];

        while (!st.stop_requested()) {
            pollfd pfd{sock_, POLLIN, 0};
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
                if (::recv(sock_, hdr, 2, MSG_WAITALL) != 2) break;
                expect = static_cast<size_t>((hdr[0] << 8) | hdr[1]);
                if (expect > kMaxPacket || expect < 4) {
                    log::error("bogus packet length {}", expect);
                    break;
                }
                buf.clear();
                buf.reserve(expect);
            } else {
                size_t want = std::min(sizeof(tmp), expect - buf.size());
                ssize_t n = ::recv(sock_, tmp, want, 0);
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                    break;
                }
                buf.append(tmp, static_cast<size_t>(n));
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

} // namespace squeeze2raop2
