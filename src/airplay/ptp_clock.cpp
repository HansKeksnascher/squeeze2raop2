#include "airplay/ptp_clock.h"

#include "airplay/ptp_packets.h"
#include "common/log.h"
#include "common/net_util.h"
#include "common/util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

namespace squeeze2raop2 {

namespace {

// CLOCK_MONOTONIC in nanoseconds: the local timebase the offset is relative to.
// Monotonic (not wall clock) so a host NTP step cannot jump the PTP timeline.
uint64_t monotonicNs() {
    timespec ts{};
    (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t randomClockId() {
    std::random_device rd;
    uint64_t v = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    return v != 0 ? v : 1;
}

// The interface's IPv4 address, or nullopt when it has none / does not exist.
std::optional<std::string> interfaceIpv4(const std::string& iface) {
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return std::nullopt;
    std::optional<std::string> out;
    for (ifaddrs* p = list; p != nullptr; p = p->ifa_next) {
        if (p->ifa_addr == nullptr || p->ifa_addr->sa_family != AF_INET) continue;
        if (iface != p->ifa_name) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
        out = ipv4ToString(sin->sin_addr);
        break;
    }
    ::freeifaddrs(list);
    return out;
}

UniqueFd bindPtpSocket(uint16_t port, const std::string& ifaceIp, bool joinMulticast,
                       std::string& err) {
    UniqueFd fd{::socket(AF_INET, SOCK_DGRAM, 0)};
    if (!fd) {
        err = errnoMessage(errno);
        return {};
    }
    int one = 1;
    (void)::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#if defined(SO_REUSEPORT)
    (void)::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        err = errnoMessage(errno);
        return {};
    }
    if (joinMulticast && !ifaceIp.empty()) {
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = ::inet_addr("224.0.1.129");  // gPTP multicast
        mreq.imr_interface.s_addr = ::inet_addr(ifaceIp.c_str());
        if (::setsockopt(fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0)
            log::debug(log::Area::Ap, "PTP multicast join on {} failed: {}", ifaceIp,
                       errnoMessage(errno));
    }
    const int flags = ::fcntl(fd.get(), F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK);
    return fd;
}

void sendTo(int fd, const std::string& ip, uint16_t port, const std::string& data) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) return;
    (void)::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

}  // namespace

struct PtpClock::Impl {
    struct Peer {
        uint64_t clockId = 0;  // receiver master identity (Announce/Follow_Up)
        bool haveSample = false;
        bool logged = false;
        uint64_t deviceTimeNs = 0;
        uint64_t localTimeNs = 0;
        int64_t offsetNs = 0;
        uint64_t rttNs = 0;
        // While probing we stay silent; if the receiver never sends Sync we
        // become grandmaster and re-assert on masterNext.
        uint64_t probeUntil = 0;
        uint64_t masterNext = 0;
        // In-flight Delay_Req exchange (t1 = receiver send, t2 = our Sync
        // receive, t3 = our Delay_Req send).
        bool haveT1 = false, haveT2 = false, haveT3 = false;
        uint64_t t1 = 0, t2 = 0, t3 = 0;
        uint16_t delayReqSeq = 0;
        bool delayReqPending = false;
    };

    Config config;
    std::string error;
    std::string localAddress;
    uint64_t clockId = 0;
    UniqueFd eventFd;
    UniqueFd generalFd;
    std::jthread thread;
    std::atomic<bool> running{false};
    uint16_t boundEventPort = 0;
    uint16_t boundGeneralPort = 0;

    mutable std::mutex mu;
    std::unordered_map<std::string, Peer> peers;
    uint16_t syncSeq = 0, announceSeq = 0, signalingSeq = 0, delaySeq = 0;

    uint64_t probeNs() const {
        return static_cast<uint64_t>(config.probeTimeout.count()) * 1000000ULL;
    }
    uint64_t masterNs() const {
        return static_cast<uint64_t>(config.masterInterval.count()) * 1000000ULL;
    }
    void run();
    void drainEvent();
    void drainGeneral();
    void sendBurst(const std::string& ip);
    void sendMasterBurst(const std::string& ip);
};

void PtpClock::Impl::run() {
    while (running.load(std::memory_order_relaxed)) {
        pollfd fds[2] = {{eventFd.get(), POLLIN, 0}, {generalFd.get(), POLLIN, 0}};
        const int pr = ::poll(fds, 2, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr > 0) {
            if ((fds[0].revents & POLLIN) != 0) drainEvent();
            if ((fds[1].revents & POLLIN) != 0) drainGeneral();
        }
        // Per-peer role: a peer that sent Sync is followed (silent); a peer
        // still inside its probe window is left alone so a standalone HomePod
        // can take over as master; only a peer that never sends Sync is served
        // as a grandmaster.
        const uint64_t now = monotonicNs();
        std::vector<std::string> masterPeers;
        {
            std::lock_guard<std::mutex> lock(mu);
            for (auto& entry : peers) {
                Peer& p = entry.second;
                if (p.haveSample || now < p.probeUntil) continue;
                if (now >= p.masterNext) {
                    p.masterNext = now + masterNs();
                    masterPeers.push_back(entry.first);
                }
            }
        }
        for (const std::string& ip : masterPeers) sendMasterBurst(ip);
    }
}

void PtpClock::Impl::drainEvent() {
    uint8_t buf[512];
    for (;;) {
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        const ssize_t n = ::recvfrom(eventFd.get(), buf, sizeof(buf), 0,
                                     reinterpret_cast<sockaddr*>(&from), &len);
        if (n <= 0) break;
        const auto data = std::span<const uint8_t>(buf, static_cast<size_t>(n));
        const std::string ip = ipv4ToString(from.sin_addr);
        const auto header = ptp::parseHeader(data);
        if (!header) continue;

        std::lock_guard<std::mutex> lock(mu);
        const auto it = peers.find(ip);
        if (it == peers.end()) continue;
        Peer& p = it->second;
        if (header->type == ptp::MessageType::Sync) {
            p.t2 = monotonicNs();
            p.haveT2 = true;
        } else if (header->type == ptp::MessageType::DelayResp) {
            const auto t4 = ptp::parseBodyTimestamp(data);
            if (!t4 || !p.haveT1 || !p.haveT2 || !p.haveT3) continue;
            // Match the Delay_Resp to the Delay_Req we sent (same sequence id).
            if (p.delayReqPending && header->sequenceId != p.delayReqSeq) continue;
            p.delayReqPending = false;
            const int64_t t1 = static_cast<int64_t>(p.t1);
            const int64_t t2 = static_cast<int64_t>(p.t2);
            const int64_t t3 = static_cast<int64_t>(p.t3);
            const int64_t t4v = static_cast<int64_t>(*t4);
            p.offsetNs = ((t1 - t2) + (t4v - t3)) / 2;  // receiver - local
            p.rttNs = static_cast<uint64_t>(std::abs((t2 - t1) - (t4v - t3)));
            p.deviceTimeNs = *t4;
            p.localTimeNs = p.t3;
            p.haveSample = true;
            p.haveT1 = p.haveT2 = p.haveT3 = false;
            log::debug(log::Area::Ap, "PTP two-way offset from {}: {} ns (rtt {} ns)", ip,
                       p.offsetNs, p.rttNs);
        }
    }
}

void PtpClock::Impl::drainGeneral() {
    uint8_t buf[512];
    for (;;) {
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        const ssize_t n = ::recvfrom(generalFd.get(), buf, sizeof(buf), 0,
                                     reinterpret_cast<sockaddr*>(&from), &len);
        if (n <= 0) break;
        const auto data = std::span<const uint8_t>(buf, static_cast<size_t>(n));
        const std::string ip = ipv4ToString(from.sin_addr);
        const auto header = ptp::parseHeader(data);
        if (!header) continue;

        if (header->type == ptp::MessageType::Announce) {
            const auto info = ptp::parseAnnounce(data);
            if (!info) continue;
            std::lock_guard<std::mutex> lock(mu);
            const auto it = peers.find(ip);
            if (it == peers.end()) continue;
            if (info->grandmasterClockId != 0) it->second.clockId = info->grandmasterClockId;
            log::debug(log::Area::Ap, "PTP Announce from {}: priority1={}, clockID={:016x}", ip,
                       info->priority1, info->grandmasterClockId);
            continue;
        }
        if (header->type != ptp::MessageType::FollowUp) continue;

        const auto t1 = ptp::parseBodyTimestamp(data);
        if (!t1) continue;
        std::lock_guard<std::mutex> lock(mu);
        const auto it = peers.find(ip);
        if (it == peers.end()) continue;
        Peer& p = it->second;
        if (header->sourceClockIdentity != 0) p.clockId = header->sourceClockIdentity;
        p.t1 = *t1;
        p.haveT1 = true;
        if (p.haveT2) {
            // One-way offset: HomePods ignore Delay_Req, and the LAN path delay
            // this ignores is sub-millisecond, far below the audio buffer.
            p.offsetNs = static_cast<int64_t>(*t1) - static_cast<int64_t>(p.t2);
            p.deviceTimeNs = *t1;
            p.localTimeNs = p.t2;
            p.rttNs = 0;
            p.haveSample = true;
            if (!p.logged) {
                p.logged = true;
                log::info(log::Area::Ap, "PTP synchronized with {}: offset={} ns (clockID={:016x})",
                          ip, p.offsetNs, p.clockId);
            }
        }
        // Two-way Delay_Req: Sonos/third-party receivers answer this form with
        // a Delay_Resp, which refines the offset (HomePods ignore Delay_Req).
        p.t3 = monotonicNs();
        p.haveT3 = true;
        p.delayReqSeq = delaySeq++;
        p.delayReqPending = true;
        const std::string req = ptp::delayRequest(clockId, 1, p.delayReqSeq, p.t3);
        sendTo(eventFd.get(), ip, ptp::kEventPort, req);
    }
}

void PtpClock::Impl::sendBurst(const std::string& ip) {
    for (int i = 0; i < 3; ++i) {
        uint16_t seq = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            seq = syncSeq++;
        }
        const auto pair = ptp::syncFollowUp(clockId, seq, monotonicNs());
        sendTo(eventFd.get(), ip, ptp::kEventPort, pair.first);
        sendTo(generalFd.get(), ip, ptp::kGeneralPort, pair.second);
        if (i < 2) {
            uint16_t aseq = 0;
            {
                std::lock_guard<std::mutex> lock(mu);
                aseq = announceSeq++;
            }
            sendTo(generalFd.get(), ip, ptp::kGeneralPort,
                   ptp::announce(clockId, aseq, config.priority1, 248));
        }
    }
    uint16_t sseq = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        sseq = signalingSeq++;
    }
    sendTo(generalFd.get(), ip, ptp::kGeneralPort, ptp::macSignaling(clockId, sseq));
}

// Grandmaster maintenance for a peer that never sends Sync: one Sync/Follow_Up
// plus an Announce, no full burst.
void PtpClock::Impl::sendMasterBurst(const std::string& ip) {
    uint16_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        seq = syncSeq++;
    }
    const auto pair = ptp::syncFollowUp(clockId, seq, monotonicNs());
    sendTo(eventFd.get(), ip, ptp::kEventPort, pair.first);
    sendTo(generalFd.get(), ip, ptp::kGeneralPort, pair.second);
    uint16_t aseq = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        aseq = announceSeq++;
    }
    sendTo(generalFd.get(), ip, ptp::kGeneralPort,
           ptp::announce(clockId, aseq, config.priority1, 248));
}

PtpClock::PtpClock() : impl_(std::make_unique<Impl>()) {}

PtpClock::~PtpClock() { stop(); }

bool PtpClock::start(const Config& config) {
    if (impl_->running.load()) return true;
    impl_->error.clear();
    impl_->clockId = randomClockId();
    impl_->localAddress.clear();
    if (!config.iface.empty()) {
        const auto ip = interfaceIpv4(config.iface);
        if (!ip) {
            impl_->error = "interface " + config.iface + " not found";
            return false;
        }
        impl_->localAddress = *ip;
    }
    impl_->config = config;
    std::string err;
    impl_->eventFd =
        bindPtpSocket(config.eventPort, impl_->localAddress, config.joinMulticast, err);
    if (!impl_->eventFd) {
        impl_->error = "bind udp/" + std::to_string(config.eventPort) + ": " + err;
        return false;
    }
    impl_->generalFd =
        bindPtpSocket(config.generalPort, impl_->localAddress, config.joinMulticast, err);
    if (!impl_->generalFd) {
        impl_->eventFd.reset();
        impl_->error = "bind udp/" + std::to_string(config.generalPort) + ": " + err;
        return false;
    }
    impl_->running.store(true);
    impl_->boundEventPort = localPort(impl_->eventFd.get());
    impl_->boundGeneralPort = localPort(impl_->generalFd.get());
    impl_->thread = std::jthread([this](std::stop_token) { impl_->run(); });
    log::info(log::Area::Ap, "PTP clock started (clockID={:016x}, iface={}, ports {}/{})",
              impl_->clockId, config.iface.empty() ? "all" : config.iface, config.eventPort,
              config.generalPort);
    return true;
}

void PtpClock::stop() {
    impl_->running.store(false);
    impl_->eventFd.shutdown();
    impl_->generalFd.shutdown();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->eventFd.reset();
    impl_->generalFd.reset();
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->peers.clear();
}

bool PtpClock::available() const { return impl_->eventFd && impl_->generalFd; }

const std::string& PtpClock::error() const { return impl_->error; }

uint64_t PtpClock::clockId() const { return impl_->clockId; }

uint16_t PtpClock::eventPort() const { return impl_->boundEventPort; }

uint16_t PtpClock::generalPort() const { return impl_->boundGeneralPort; }

std::string PtpClock::localAddress() const { return impl_->localAddress; }

std::string PtpClock::addPeer(const std::string& host) {
    // Peer keys must be numeric IPv4: received datagrams are keyed by source
    // address, so a hostname target has to be resolved before it can match.
    const auto addr = resolveIpv4(host);
    const std::string ip = addr ? ipv4ToString(*addr) : host;
    if (!impl_->running.load()) return ip;
    const uint64_t probe = monotonicNs() + impl_->probeNs();
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        Impl::Peer& p = impl_->peers.try_emplace(ip).first->second;
        p.probeUntil = probe;
        p.masterNext = probe;
        p.haveSample = false;
    }
    impl_->sendBurst(ip);
    return ip;
}

void PtpClock::removePeer(const std::string& peerIp) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->peers.erase(peerIp);
}

bool PtpClock::synchronized(const std::string& peerIp) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->peers.find(peerIp);
    return it != impl_->peers.end() && it->second.haveSample;
}

uint64_t PtpClock::nowNs(const std::string& peerIp) const {
    const uint64_t local = monotonicNs();
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->peers.find(peerIp);
    if (it != impl_->peers.end() && it->second.haveSample)
        return static_cast<uint64_t>(static_cast<int64_t>(local) + it->second.offsetNs);
    return local;
}

uint64_t PtpClock::peerClockId(const std::string& peerIp) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->peers.find(peerIp);
    if (it != impl_->peers.end() && it->second.clockId != 0) return it->second.clockId;
    return impl_->clockId;
}

std::optional<PtpClock::Sample> PtpClock::sample(const std::string& peerIp) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const auto it = impl_->peers.find(peerIp);
    if (it == impl_->peers.end() || !it->second.haveSample) return std::nullopt;
    Sample s;
    s.peerClockId = it->second.clockId;
    s.deviceTimeNs = it->second.deviceTimeNs;
    s.localTimeNs = it->second.localTimeNs;
    s.offsetNs = it->second.offsetNs;
    s.rttNs = it->second.rttNs;
    return s;
}

}  // namespace squeeze2raop2
