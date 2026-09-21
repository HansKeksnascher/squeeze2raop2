#include "slimproto.h"

#include "log.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

// Wire half of the slimproto client: opcode/packet encoding (HELO, STAT,
// RESP, SETD, DSCO, META) and decoding (the process() dispatch, including
// the AUDG gain -> slider percent inversion), plus the PCM parameter code
// tables. Connection lifecycle lives in slimproto.cpp.

namespace squeeze2raop2 {

namespace {

// Big-endian field access into assembled/received packets, replacing the
// as_writable_bytes(std::span{pkt}).subspan() ceremony at every call site.
std::span<std::byte> beField(std::vector<uint8_t>& pkt, size_t off, size_t len) {
    return std::as_writable_bytes(std::span{pkt}).subspan(off, len);
}
std::span<const std::byte> beField(const std::string& pkt, size_t off, size_t len) {
    return std::as_bytes(std::span{pkt}).subspan(off, len);
}
uint64_t beAt(const std::string& pkt, size_t off, size_t len) {
    return unpackN(beField(pkt, off, len));
}

// Opcodes LMS sends that this client intentionally ignores.
constexpr std::array<std::string_view, 10> kIgnoredOps{
    "aude", "DBUG", "SYST", "visu", "IR  ", "GRFe", "GRFh", "GRFb", "GRFm", "OCOB"};

} // namespace

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

void SlimProtoClient::sendHelo(bool reconnect) {
    std::string caps = caps_;
    uint32_t bodyLen = 36 + static_cast<uint32_t>(caps.size());
    std::vector<uint8_t> pkt(8 + bodyLen);
    std::memcpy(pkt.data(), "HELO", 4);
    packN(beField(pkt, 4, 4), bodyLen, 4);
    std::span<std::byte> p = beField(pkt, 8, pkt.size() - 8);
    p[0] = std::byte{12};   // deviceid 12 = squeezeplay class (squeezelite parity)
    p[1] = std::byte{1};    // revision: single byte, shown as player firmware rev
    std::memcpy(p.data() + 2, mac_.data(), 6);
    packN(p.subspan(24, 2), reconnect ? 0x4000 : 0x0000, 2);
    std::memcpy(p.data() + 36, caps.data(), caps.size());

    log::info("HELO mac={} cap={}", macToString(mac_), caps);
    if (!sendRaw(std::as_bytes(std::span{pkt}))) log::error("HELO send failed");
}


StreamStats SlimProtoClient::lastStats() {
    std::lock_guard<std::mutex> lock(sendMutex_);
    return stats_;
}

void SlimProtoClient::sendStat(const char* event, StreamStats stats,
                               uint32_t serverTimestamp) {
    {
        // stats_ is read back by the run thread ('f'/'p'/'u' handlers) and
        // written from stream threads; sendMutex_ serializes both.
        std::lock_guard<std::mutex> lock(sendMutex_);
        stats_ = stats;
    }
    std::vector<uint8_t> pkt(8 + 53);
    std::memcpy(pkt.data(), "STAT", 4);
    packN(beField(pkt, 4, 4), 53, 4);
    std::span<std::byte> p = beField(pkt, 8, 53);
    std::memcpy(p.data(), event, 4);
    p[6] = std::byte{0};
    packN(p.subspan(7, 4), stats.streamBufferSize, 4);
    packN(p.subspan(11, 4), stats.streamBufferFullness, 4);
    packN(p.subspan(15, 4), stats.bytesReceived >> 32, 4);
    packN(p.subspan(19, 4), stats.bytesReceived & 0xFFFFFFFF, 4);
    packN(p.subspan(23, 2), 0xFFFF, 2);
    packN(p.subspan(25, 4), nowMs(), 4);
    packN(p.subspan(29, 4), stats.outputBufferSize, 4);
    packN(p.subspan(33, 4), stats.outputBufferFullness, 4);
    packN(p.subspan(37, 4), stats.elapsedMs / 1000, 4);
    packN(p.subspan(41, 2), 0, 2);
    packN(p.subspan(43, 4), stats.elapsedMs, 4);
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

void SlimProtoClient::sendMeta(std::string_view data) {
    // squeezelite parity: forward the raw ICY metadata block to LMS so its
    // track display follows the stream (LMS also watches direct streams
    // itself; this is redundant there but keeps proxied streams in sync).
    // The ICY de-interleaver only invokes this with a non-empty block; an
    // empty block carries no information for LMS either way.
    if (data.empty()) return;
    (void)sendPacket("META", std::as_bytes(std::span{data}));   // best effort:
    // metadata is cosmetic; connection health is the read loop's job
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
            uint32_t ts = static_cast<uint32_t>(beAt(pkt, 18, 4));
            sendStat("STMt", {}, ts);
            lastHeartbeatMs_ = nowMs();
            break;
        }
        case 'q':
            if (events_.onStop) events_.onStop();
            break;
        case 'f':
            if (events_.onFlush) events_.onFlush(true);
            sendStat("STMf", lastStats());
            break;
        case 'p': {
            if (len < 22) return;
            uint32_t ms = static_cast<uint32_t>(beAt(pkt, 18, 4));
            if (events_.onPause) events_.onPause(ms);
            if (!ms) sendStat("STMp", lastStats());
            break;
        }
        case 'a': {
            if (len < 22) return;
            uint32_t ms = static_cast<uint32_t>(beAt(pkt, 18, 4));
            if (events_.onSkipAhead) events_.onSkipAhead(ms);
            break;
        }
        case 'u': {
            if (len < 22) return;
            uint32_t jiffies = static_cast<uint32_t>(beAt(pkt, 18, 4));
            if (events_.onUnpause) events_.onUnpause(jiffies);
            sendStat("STMr", lastStats());
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
            st.replayGain = static_cast<uint32_t>(beAt(pkt, 18, 4));
            st.serverPort = static_cast<uint16_t>(beAt(pkt, 22, 2));
            st.serverIp = static_cast<uint32_t>(beAt(pkt, 24, 4));
            st.request.assign(pkt.data() + 28, len - 28);
            log::debug("strm s autostart={} format={} threshold={}", st.autostart,
                       static_cast<char>(st.format), st.thresholdKb);
            sendStat("STMf", lastStats());
            if (events_.onStart) events_.onStart(st);
            break;
        }
        default:
            log::warn("unhandled strm command '{}'", command);
            break;
        }
    } else if (op == "cont") {
        if (len < 8) return;
        uint32_t metaint = static_cast<uint32_t>(beAt(pkt, 4, 4));
        if (events_.onCont) events_.onCont(metaint);
    } else if (op == "codc") {
        if (len < 10) return;
        StreamFormat f = static_cast<StreamFormat>(pkt[4]);
        PcmParams pcm{static_cast<uint8_t>(pkt[5]), static_cast<uint8_t>(pkt[6]),
                      static_cast<uint8_t>(pkt[7]), static_cast<uint8_t>(pkt[8])};
        if (events_.onCodc) events_.onCodc(f, pcm);
    } else if (op == "audg") {
        if (len < 22) return;
        uint32_t gainL = static_cast<uint32_t>(beAt(pkt, 14, 4));
        uint32_t gainR = static_cast<uint32_t>(beAt(pkt, 18, 4));
        uint8_t adjust = static_cast<uint8_t>(pkt[12]);
        // dvc=0 is LMS's fixed-output mode: the gains are the no-op 1.0 and
        // applying them would push 0 dB = full blast. Leave the receiver at
        // its current level.
        if (!adjust) {
            log::debug("audg dvc=0 ignored (fixed-output mode)");
            return;
        }
        // new_left/new_right are 16.16 fixed-point linear amplitude
        // multipliers (1.0 = full volume). Invert LMS's linear dB slider
        // curve (Squeezebox2 getVolume: 0.495 dB/step over -50..0 dB,
        // maximumVolume 0) to recover the slider percent:
        // pct = 100 + dB*101/50. The bridge passes this straight to the
        // AirPlay sender's 0..100 % domain, whose 0 % is the -144 mute
        // sentinel and 100 % is 0 dB: the full LMS slider span maps onto
        // AirPlay's -30..0 dB protocol range at 0.3 dB per slider step
        // (LMS minimum = receiver mute, LMS maximum = full scale).
        auto pctOf = [&](uint32_t raw) {
            if (raw == 0) return 0.0;              // LMS mute
            double db = 20.0 * std::log10(static_cast<double>(raw) / 65536.0);
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
            uint32_t ip = static_cast<uint32_t>(beAt(pkt, 4, 4));
            if (events_.onServerSwitch) events_.onServerSwitch(ip);
        }
    } else if (std::ranges::find(kIgnoredOps, op) != kIgnoredOps.end()) {
        log::debug("ignored {}", op);
    } else {
        log::warn("unhandled opcode {}", op);
    }
}

} // namespace squeeze2raop2
