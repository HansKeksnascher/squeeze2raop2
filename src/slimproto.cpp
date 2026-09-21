#include "slimproto.h"

#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <span>

namespace squeeze2raop2 {

namespace {

constexpr size_t kMaxPacket = size_t{4096} * 8;
constexpr int kPollTimeoutMs = 100;

std::string ipv4ToString(const in_addr& addr) {
    char buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr, buf, sizeof(buf))) return std::string();
    return std::string(buf);
} // namespace

} // namespace

bool discoverLms(std::string& hostOut, uint16_t port, uint32_t timeoutMs) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log::error("discovery socket failed: {}", strerror(errno));
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
            log::warn("discovery send failed: {}", strerror(errno));

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
} // namespace squeeze2raop2

uint32_t sampleRateFromCode(uint8_t code) {
    switch (code) {
    case '0': return 11000;
    case '1': return 22000;
    case '2': return 32000;
    case '3': return 44100;
    case '4': return 48000;
    case '5': return 8000;
    case '6': return 12000;
    case '7': return 16000;
    case '8': return 24000;
    case '9': return 96000;
    default: return 44100;
    }
}

uint8_t bitsPerSampleFromCode(uint8_t code) {
    switch (code) {
    case '0': return 8;
    case '1': return 16;
    case '2': return 20;
    case '3': return 32;
    default: return 16;
    }
}

uint8_t channelsFromCode(uint8_t code) {
    switch (code) {
    case '1': return 1;
    default: return 2;   // '2' and anything unknown: stereo
    }
}

PcmFormat pcmFormat(const PcmParams& params, uint32_t fallbackRate) {
    PcmFormat f;
    if (params.sampleSizeCode != '?') f.bitsPerSample = bitsPerSampleFromCode(params.sampleSizeCode);
    if (params.sampleRateCode != '?') f.sampleRate = sampleRateFromCode(params.sampleRateCode);
    else f.sampleRate = fallbackRate;
    if (params.channelsCode != '?') f.channels = channelsFromCode(params.channelsCode);
    if (params.endianCode != '?') f.bigEndian = (params.endianCode == '0');
    return f;
}

SlimProtoClient::SlimProtoClient(std::array<uint8_t, 6> mac, std::string caps, Events events)
    : mac_(mac), caps_(std::move(caps)), events_(std::move(events)) {}

SlimProtoClient::~SlimProtoClient() { stop(); }

void SlimProtoClient::setPlayerName(const std::string& name) { playerName_ = name; }

void SlimProtoClient::start(const std::string& host, uint16_t port) {
    host_ = host;
    port_ = port;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void SlimProtoClient::stop() {
    if (running_.exchange(false)) {
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
    const char* p = reinterpret_cast<const char*>(data.data());
    size_t len = data.size();
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(sock_, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR || errno == ENOBUFS)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return sent == len;
}

bool SlimProtoClient::sendPacket(std::string_view opcode, std::span<const std::byte> payload) {
    if (sock_ < 0) return false;
    // client -> LMS framing (per squeezelite/HELO spec):
    // [4b opcode][4b big-endian length = payload bytes][payload]
    std::vector<uint8_t> pkt(8 + payload.size());
    std::memcpy(pkt.data(), opcode.data(), 4);
    packN(std::as_writable_bytes(std::span{pkt}).subspan(4, 4), payload.size(), 4);
    if (!payload.empty())
        std::memcpy(pkt.data() + 8, payload.data(), payload.size());
    return sendRaw(std::as_bytes(std::span{pkt}));
}

void SlimProtoClient::sendHelo(bool reconnect) {
    std::string caps = caps_;
    uint32_t bodyLen = 36 + static_cast<uint32_t>(caps.size());
    std::vector<uint8_t> pkt(8 + bodyLen);
    std::memcpy(pkt.data(), "HELO", 4);
    packN(std::as_writable_bytes(std::span{pkt}).subspan(4, 4), bodyLen, 4);
    std::span<std::byte> p = std::as_writable_bytes(std::span{pkt}).subspan(8);
    p[0] = std::byte{12};
    p[1] = std::byte{0};
    std::memcpy(p.data() + 2, mac_.data(), 6);
    packN(p.subspan(24, 2), reconnect ? 0x4000 : 0x0000, 2);
    std::memcpy(p.data() + 36, caps.data(), caps.size());

    log::info("HELO mac={} cap={}", macToString(mac_), caps);
    if (!sendRaw(std::as_bytes(std::span{pkt}))) log::error("HELO send failed");
}


void SlimProtoClient::sendStat(const char* event, StreamStats stats,
                               uint32_t serverTimestamp) {
    stats_ = stats;
    std::vector<uint8_t> pkt(8 + 53);
    std::memcpy(pkt.data(), "STAT", 4);
    packN(std::as_writable_bytes(std::span{pkt}).subspan(4, 4), 53, 4);
    std::span<std::byte> p = std::as_writable_bytes(std::span{pkt}).subspan(8);
    std::memcpy(p.data(), event, 4);
    p[6] = std::byte{0};
    packN(p.subspan(7, 4), stats_.streamBufferSize, 4);
    packN(p.subspan(11, 4), stats_.streamBufferFullness, 4);
    packN(p.subspan(15, 4), stats_.bytesReceived >> 32, 4);
    packN(p.subspan(19, 4), stats_.bytesReceived & 0xFFFFFFFF, 4);
    packN(p.subspan(23, 2), 0xFFFF, 2);
    packN(p.subspan(25, 4), nowMs(), 4);
    packN(p.subspan(29, 4), stats_.outputBufferSize, 4);
    packN(p.subspan(33, 4), stats_.outputBufferFullness, 4);
    packN(p.subspan(37, 4), stats_.elapsedMs / 1000, 4);
    packN(p.subspan(41, 2), 0, 2);
    packN(p.subspan(43, 4), stats_.elapsedMs, 4);
    packN(p.subspan(47, 4), serverTimestamp, 4);
    packN(p.subspan(51, 2), 0, 2);
    if (!sendRaw(std::as_bytes(std::span{pkt}))) log::warn("STAT send failed");
}

void SlimProtoClient::sendResp(const std::string& header) {
    if (!sendPacket("RESP", std::as_bytes(std::span{header}))) log::warn("RESP send failed");
}

void SlimProtoClient::sendSetdName(const std::string& name) {
    std::vector<uint8_t> payload(1 + name.size() + 1);
    payload[0] = 0;
    std::memcpy(payload.data() + 1, name.data(), name.size());
    if (!sendPacket("SETD", std::as_bytes(std::span{payload}))) log::warn("SETD send failed");
}

void SlimProtoClient::sendDisco(uint8_t reason) {
    if (!sendPacket("DSCO", std::as_bytes(std::span{&reason, 1}))) log::warn("DSCO send failed");
}

void SlimProtoClient::sendMeta(const char* data, size_t len) {
    // squeezelite parity: forward the raw ICY metadata block to LMS so its
    // track display follows the stream (LMS also watches direct streams
    // itself; this is redundant there but keeps proxied streams in sync).
    sendPacket("META", std::as_bytes(std::span{data, len}));
}

bool SlimProtoClient::connectOnce(bool reconnect) {
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &sa.sin_addr) != 1) {
        // getaddrinfo instead of gethostbyname2: the latter returns a
        // thread-unsafe static hostent, and multiple sessions resolve
        // concurrently here.
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host_.c_str(), nullptr, &hints, &res) != 0 || !res) {
            log::error("cannot resolve {}", host_);
            return false;
        }
        const auto* ai = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
        sa.sin_addr = ai->sin_addr;
        freeaddrinfo(res);
    }
    sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) return false;
    int one = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // Server-dead resilience (squeezelite's 35 s watchdog parity): kernel
    // keepalive detects a half-open control connection (~2.5 min to fail
    // with these settings) so run() reconnects instead of blocking forever.
    setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int kaIdle = 30, kaIntvl = 10, kaCnt = 6;
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPIDLE, &kaIdle, sizeof(kaIdle));
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPINTVL, &kaIntvl, sizeof(kaIntvl));
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPCNT, &kaCnt, sizeof(kaCnt));
    if (::connect(sock_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        ::close(sock_);
        sock_ = -1;
        return false;
    }
    sendHelo(reconnect);
    return true;
}

void SlimProtoClient::process(const std::string& pkt) {
    size_t len = pkt.size();
    if (len < 4) return;
    const std::string_view op(pkt.data(), 4);

    if (op == "strm") {
        if (len < 5) return;
        const char command = pkt[4];
        switch (command) {
        case 't': {
            // heartbeat timestamp sits at packet bytes 18..21
            if (len < 22) return;
            uint32_t ts = static_cast<uint32_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(18, 4)));
            sendStat("STMt", {}, ts);
            lastHeartbeatMs_ = nowMs();
            break;
        }
        case 'q':
            if (events_.onStop) events_.onStop();
            break;
        case 'f':
            if (events_.onFlush) events_.onFlush(true);
            sendStat("STMf", stats_);
            break;
        case 'p': {
            if (len < 22) return;
            uint32_t ms = static_cast<uint32_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(18, 4)));
            if (events_.onPause) events_.onPause(ms);
            if (!ms) sendStat("STMp", stats_);
            break;
        }
        case 'a': {
            if (len < 22) return;
            uint32_t ms = static_cast<uint32_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(18, 4)));
            if (events_.onSkipAhead) events_.onSkipAhead(ms);
            break;
        }
        case 'u': {
            if (len < 22) return;
            uint32_t jiffies = static_cast<uint32_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(18, 4)));
            if (events_.onUnpause) events_.onUnpause(jiffies);
            sendStat("STMr", stats_);
            break;
        }
        case 's': {
            if (len < 28) return;
            StrmStart st;
            st.autostart = static_cast<uint8_t>(pkt[5] - '0');
            st.format = static_cast<StreamFormat>(pkt[6]);
            st.pcm.sampleSizeCode = static_cast<uint8_t>(pkt[7]);
            st.pcm.sampleRateCode = static_cast<uint8_t>(pkt[8]);
            st.pcm.channelsCode = static_cast<uint8_t>(pkt[9]);
            st.pcm.endianCode = static_cast<uint8_t>(pkt[10]);
            st.thresholdKb = static_cast<uint8_t>(pkt[11]);
            st.transitionPeriodS = static_cast<uint8_t>(pkt[13]);
            st.transitionType = static_cast<uint8_t>(pkt[14] - '0');
            st.flags = static_cast<uint8_t>(pkt[15]);
            st.outputThresholdTenths = static_cast<uint8_t>(pkt[16]);
            st.replayGain = static_cast<uint32_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(18, 4)));
            st.serverPort = static_cast<uint16_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(22, 2)));
            st.serverIp = static_cast<uint32_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(24, 4)));
            st.request.assign(pkt.data() + 28, len - 28);
            log::debug("strm s autostart={} format={} threshold={}", st.autostart,
                       static_cast<char>(st.format), st.thresholdKb);
            sendStat("STMf", stats_);
            if (events_.onStart) events_.onStart(st);
            break;
        }
        default:
            log::warn("unhandled strm command '{}'", command);
            break;
        }
    } else if (op == "cont") {
        if (len < 8) return;
        uint32_t metaint = static_cast<uint32_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(4, 4)));
        if (events_.onCont) events_.onCont(metaint);
    } else if (op == "codc") {
        if (len < 10) return;
        StreamFormat f = static_cast<StreamFormat>(pkt[4]);
        PcmParams pcm{static_cast<uint8_t>(pkt[5]), static_cast<uint8_t>(pkt[6]),
                      static_cast<uint8_t>(pkt[7]), static_cast<uint8_t>(pkt[8])};
        if (events_.onCodc) events_.onCodc(f, pcm);
    } else if (op == "audg") {
        if (len < 22) return;
        uint32_t gainL = static_cast<uint32_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(14, 4)));
        uint32_t gainR = static_cast<uint32_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(18, 4)));
        uint8_t adjust = static_cast<uint8_t>(pkt[12]);
        // new_left/new_right are 16.16 fixed-point linear amplitude
        // multipliers (1.0 = full volume). LMS encodes its slider through a
        // linear dB curve (Squeezebox2 getVolume: 0.495 dB/step over
        // -50..0 dB, maximumVolume 0). Invert it to recover the slider
        // percent: pct = 100 + dB*101/50, then hand that to the AirPlay
        // sender whose 0..100 % mapping covers the protocol's -30..0 dB
        // range (0 % = -144 mute sentinel). AirPlay cannot go below -30 dB,
        // so LMS gain below ~slider 40 falls at pct 0 (clamped), which the
        // receiver plays as its quietest level; LMS mute (gain 0) -> pct 0.
        auto pctOf = [&](uint32_t raw) {
            uint32_t gain = adjust ? raw : 65536;  // dvc=0 => full volume
            if (gain == 0) return 0.0;             // LMS mute
            double db = 20.0 * std::log10(static_cast<double>(gain) / 65536.0);
            double pct = 100.0 + db * 101.0 / 50.0;
            return std::clamp(pct, 0.0, 100.0);
        };
        if (events_.onVolume) events_.onVolume(pctOf(gainL), pctOf(gainR));
    } else if (op == "setd") {
        if (len >= 5 && pkt[4] == '\0') {
            if (len == 5) {
                sendSetdName(playerName_.empty() ? "squeeze2raop2" : playerName_);
            } else if (len > 5) {
                std::string name(pkt.data() + 5, len - 5);
                while (!name.empty() && name.back() == '\0') name.pop_back();
                if (events_.onSetName) events_.onSetName(name);
                sendSetdName(name);
            }
        }
    } else if (op == "serv") {
        if (len >= 8) {
            uint32_t ip = static_cast<uint32_t>(unpackN(std::as_bytes(std::span{pkt}).subspan(4, 4)));
            if (events_.onServerSwitch) events_.onServerSwitch(ip);
        }
    } else if (op == "aude" || op == "DBUG" || op == "SYST" || op == "visu" ||
               op == "IR  " || op == "GRFe" || op == "GRFh" || op == "GRFb" ||
               op == "GRFm" || op == "OCOB") {
        log::debug("ignored {}", op);
    } else {
        log::warn("unhandled opcode {}", op);
    }
}

void SlimProtoClient::maybeHeartbeat() {
    uint64_t now = nowMs();
    if (now - lastHeartbeatMs_ >= 1000) {
        lastHeartbeatMs_ = now;
        if (statsProvider_) sendStat("STMt", statsProvider_());
        else sendStat("STMt", {});
    }
}

void SlimProtoClient::run() {
    unsigned fails = 0;
    while (running_.load()) {
        if (host_.empty() && !discoverLms(host_, port_, 5000)) {
            log::warn("LMS discovery failed, retrying in 5s");
            for (unsigned i = 0; i < 50 && running_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!connectOnce(reconnect_)) {
            ++fails;
            unsigned delay = std::min<unsigned>(fails * 2, 15);
            log::warn("connect to {} failed, retrying in {}s", host_, delay);
            for (unsigned i = 0; i < delay * 10 && running_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        fails = 0;
        reconnect_ = true;

        std::string buf;
        size_t expect = 0;
        char tmp[2048];

        while (running_.load()) {
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
        if (!running_.load()) break;
    }
}

}
