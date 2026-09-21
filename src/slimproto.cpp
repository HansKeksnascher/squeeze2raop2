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
#include <cmath>
#include <cstring>

namespace sq2 {

namespace {

constexpr size_t kMaxPacket = 4096 * 8;
constexpr int kPollTimeoutMs = 100;

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
                buf[n] = '\0';
                if (buf[0] == 'E' || buf[0] == 'D') {
                    ::close(fd);
                    hostOut = inet_ntoa(from.sin_addr);
                    log::info("discovered LMS at {}:{}", hostOut, port);
                    return true;
                }
            }
        }
    }
    ::close(fd);
    return false;
}

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
    case '2': return 2;
    default: return 2;
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
        sendPacket("BYE!", &bye, 1);
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

bool SlimProtoClient::sendRaw(const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    const uint8_t* p = static_cast<const uint8_t*>(data);
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

bool SlimProtoClient::sendPacket(const char* opcode, const void* payload, size_t len) {
    if (sock_ < 0) return false;
    // client -> LMS framing (per squeezelite/HELO spec):
    // [4b opcode][4b big-endian length = payload bytes][payload]
    std::vector<uint8_t> pkt(8 + len);
    memcpy(pkt.data(), opcode, 4);
    pkt[4] = static_cast<uint8_t>(len >> 24);
    pkt[5] = static_cast<uint8_t>(len >> 16);
    pkt[6] = static_cast<uint8_t>(len >> 8);
    pkt[7] = static_cast<uint8_t>(len & 0xFF);
    if (len) memcpy(pkt.data() + 8, payload, len);
    return sendRaw(pkt.data(), pkt.size());
}

void SlimProtoClient::sendHelo(bool reconnect) {
    std::string caps = caps_;
    uint32_t bodyLen = 36 + caps.size();
    std::vector<uint8_t> pkt(8 + bodyLen);
    memcpy(pkt.data(), "HELO", 4);
    pkt[4] = static_cast<uint8_t>(bodyLen >> 24);
    pkt[5] = static_cast<uint8_t>(bodyLen >> 16);
    pkt[6] = static_cast<uint8_t>(bodyLen >> 8);
    pkt[7] = static_cast<uint8_t>(bodyLen & 0xFF);
    uint8_t* p = pkt.data() + 8;
    p[0] = 12;
    p[1] = 0;
    memcpy(p + 2, mac_.data(), 6);
    packN(p + 24, reconnect ? 0x4000 : 0x0000, 2);
    memcpy(p + 36, caps.data(), caps.size());

    log::info("HELO mac={} cap={}", macToString(mac_), caps);
    if (!sendRaw(pkt.data(), pkt.size())) log::error("HELO send failed");
}


void SlimProtoClient::sendStat(const char* event, StreamStats stats,
                               uint32_t serverTimestamp) {
    stats_ = stats;
    std::vector<uint8_t> pkt(8 + 53);
    memcpy(pkt.data(), "STAT", 4);
    pkt[4] = 0;
    pkt[5] = 0;
    pkt[6] = 0;
    pkt[7] = 53;
    uint8_t* p = pkt.data() + 8;
    memcpy(p, event, 4);
    p[6] = 0;
    packN(p + 7, stats_.streamBufferSize, 4);
    packN(p + 11, stats_.streamBufferFullness, 4);
    packN(p + 15, stats_.bytesReceived >> 32, 4);
    packN(p + 19, stats_.bytesReceived & 0xFFFFFFFF, 4);
    packN(p + 23, 0xFFFF, 2);
    packN(p + 25, nowMs(), 4);
    packN(p + 29, stats_.outputBufferSize, 4);
    packN(p + 33, stats_.outputBufferFullness, 4);
    packN(p + 37, stats_.elapsedMs / 1000, 4);
    packN(p + 41, 0, 2);
    packN(p + 43, stats_.elapsedMs, 4);
    memcpy(p + 47, &serverTimestamp, 4);
    packN(p + 51, 0, 2);
    if (!sendRaw(pkt.data(), pkt.size())) log::warn("STAT send failed");
}

void SlimProtoClient::sendResp(const std::string& header) {
    if (!sendPacket("RESP", header.data(), header.size())) log::warn("RESP send failed");
}

void SlimProtoClient::sendSetdName(const std::string& name) {
    std::vector<uint8_t> payload(1 + name.size() + 1);
    payload[0] = 0;
    memcpy(payload.data() + 1, name.data(), name.size());
    if (!sendPacket("SETD", payload.data(), payload.size())) log::warn("SETD send failed");
}

void SlimProtoClient::sendDisco(uint8_t reason) {
    if (!sendPacket("DSCO", &reason, 1)) log::warn("DSCO send failed");
}

void SlimProtoClient::sendMeta(const char* data, size_t len) {
    // squeezelite parity: forward the raw ICY metadata block to LMS so its
    // track display follows the stream (LMS also watches direct streams
    // itself; this is redundant there but keeps proxied streams in sync).
    sendPacket("META", data, len);
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
        hostent* he = gethostbyname2(host_.c_str(), AF_INET);
        if (!he || !he->h_addr_list[0]) {
            log::error("cannot resolve {}", host_);
            return false;
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
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
    const uint8_t* p = reinterpret_cast<const uint8_t*>(pkt.data());
    size_t len = pkt.size();
    if (len < 4) return;
    char op[5];
    memcpy(op, pkt.data(), 4);
    op[4] = 0;

    if (strncmp(op, "strm", 4) == 0) {
        if (len < 5) return;
        const uint8_t* d = p + 4;
        char command = static_cast<char>(d[0]);
        switch (command) {
        case 't': {
            uint32_t ts = static_cast<uint32_t>(unpackN(d + 14, 4));
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
            uint32_t ms = static_cast<uint32_t>(unpackN(d + 14, 4));
            if (events_.onPause) events_.onPause(ms);
            if (!ms) sendStat("STMp", stats_);
            break;
        }
        case 'a': {
            uint32_t ms = static_cast<uint32_t>(unpackN(d + 14, 4));
            if (events_.onSkipAhead) events_.onSkipAhead(ms);
            break;
        }
        case 'u': {
            uint32_t jiffies = static_cast<uint32_t>(unpackN(d + 14, 4));
            if (events_.onUnpause) events_.onUnpause(jiffies);
            sendStat("STMr", stats_);
            break;
        }
        case 's': {
            if (len < 28) return;
            StrmStart st;
            st.autostart = static_cast<uint8_t>(d[1] - '0');
            st.format = static_cast<StreamFormat>(d[2]);
            st.pcm.sampleSizeCode = d[3];
            st.pcm.sampleRateCode = d[4];
            st.pcm.channelsCode = d[5];
            st.pcm.endianCode = d[6];
            st.thresholdKb = d[7];
            st.transitionPeriodS = d[9];
            st.transitionType = static_cast<uint8_t>(d[10] - '0');
            st.flags = d[11];
            st.outputThresholdTenths = d[12];
            st.replayGain = static_cast<uint32_t>(unpackN(d + 14, 4));
            st.serverPort = static_cast<uint16_t>(unpackN(d + 18, 2));
            st.serverIp = static_cast<uint32_t>(unpackN(d + 20, 4));
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
    } else if (strncmp(op, "cont", 4) == 0) {
        if (len < 8) return;
        uint32_t metaint = static_cast<uint32_t>(unpackN(p + 4, 4));
        if (events_.onCont) events_.onCont(metaint);
    } else if (strncmp(op, "codc", 4) == 0) {
        if (len < 10) return;
        StreamFormat f = static_cast<StreamFormat>(p[4]);
        PcmParams pcm{p[5], p[6], p[7], p[8]};
        if (events_.onCodc) events_.onCodc(f, pcm);
    } else if (strncmp(op, "audg", 4) == 0) {
        if (len < 18) return;
        uint32_t gainL = static_cast<uint32_t>(unpackN(p + 14, 4));
        uint32_t gainR = static_cast<uint32_t>(unpackN(p + 18, 4));
        uint8_t adjust = p[12];
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
    } else if (strncmp(op, "setd", 4) == 0) {
        if (len >= 5 && p[4] == 0) {
            if (len == 5) {
                sendSetdName(playerName_.empty() ? "sqraop2" : playerName_);
            } else if (len > 5) {
                std::string name(pkt.data() + 5, len - 5);
                while (!name.empty() && name.back() == '\0') name.pop_back();
                if (events_.onSetName) events_.onSetName(name);
                sendSetdName(name);
            }
        }
    } else if (strncmp(op, "serv", 4) == 0) {
        if (len >= 8) {
            uint32_t ip = static_cast<uint32_t>(unpackN(p + 4, 4));
            if (events_.onServerSwitch) events_.onServerSwitch(ip);
        }
    } else if (strncmp(op, "aude", 4) == 0 || strncmp(op, "DBUG", 4) == 0 ||
               strncmp(op, "SYST", 4) == 0 || strncmp(op, "visu", 4) == 0 ||
               strncmp(op, "IR  ", 4) == 0 || strncmp(op, "GRFe", 4) == 0 ||
               strncmp(op, "GRFh", 4) == 0 || strncmp(op, "GRFb", 4) == 0 ||
               strncmp(op, "GRFm", 4) == 0 || strncmp(op, "OCOB", 4) == 0) {
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
