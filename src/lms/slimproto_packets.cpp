#include "lms/slimproto.h"

#include "common/byte_order.h"
#include "common/log.h"
#include "common/util.h"
#include "playback/volume_map.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Wire half of the slimproto client: opcode/packet encoding (HELO, STAT,
// RESP, SETD, DSCO, META) and decoding (the process() dispatch, including
// the AUDG gain -> slider percent inversion), plus the PCM parameter code
// tables. Connection lifecycle lives in slimproto.cpp.
//
// Every slimproto integer field is big-endian. PacketWriter/PacketReader
// append/consume fields sequentially so the packet layout is expressed by
// field order, not by hand-kept byte offsets; PacketReader also bounds-checks
// every read (earlier offset arithmetic caused an out-of-bounds regression,
// pinned by the short-packet unit test).

namespace squeeze2raop2 {

namespace {

// Appends big-endian fields to a packet body in wire order.
class PacketWriter {
public:
    // Reserve the exact body size so the writer never reallocates mid-packet
    // (also keeps GCC's vector-growth analysis quiet).
    explicit PacketWriter(size_t reserve) { out_.reserve(reserve); }

    void u8(uint8_t value) { out_.push_back(static_cast<std::byte>(value)); }
    void u16(uint16_t value) { put(value); }
    void u32(uint32_t value) { put(value); }

    // Reserved / padding field: emitted as zero bytes.
    void skip(size_t n) { out_.insert(out_.end(), n, std::byte{0}); }

    void bytes(std::span<const std::byte> data) {
        out_.insert(out_.end(), data.begin(), data.end());
    }

    [[nodiscard]] std::span<const std::byte> data() const { return out_; }

private:
    template <std::unsigned_integral T>
    void put(T value) {
        std::array<std::byte, sizeof(T)> tmp{};
        writeInt<Endian::Big>(tmp.data(), value);
        out_.insert(out_.end(), tmp.begin(), tmp.end());
    }

    std::vector<std::byte> out_;
};

// Sequential big-endian cursor over a received packet. Reads past the end
// return nullopt; skip() reports failure.
class PacketReader {
public:
    explicit PacketReader(std::span<const std::byte> packet) : packet_(packet) {}

    [[nodiscard]] size_t remaining() const { return packet_.size() - offset_; }

    std::optional<uint8_t> u8() { return take<uint8_t>(); }
    std::optional<uint16_t> u16() { return take<uint16_t>(); }
    std::optional<uint32_t> u32() { return take<uint32_t>(); }

    // Read a big-endian u32 at an absolute packet offset without moving the
    // cursor; nullopt when the field is truncated.
    [[nodiscard]] std::optional<uint32_t> u32At(size_t offset) const {
        if (offset + sizeof(uint32_t) > packet_.size()) return std::nullopt;
        return readInt<Endian::Big, uint32_t>(packet_.data() + offset);
    }

    bool skip(size_t n) {
        if (n > remaining()) return false;
        offset_ += n;
        return true;
    }

    [[nodiscard]] std::span<const std::byte> tail() const { return packet_.subspan(offset_); }

private:
    template <std::unsigned_integral T>
    std::optional<T> take() {
        if (sizeof(T) > remaining()) return std::nullopt;
        const T value = readInt<Endian::Big, T>(packet_.data() + offset_);
        offset_ += sizeof(T);
        return value;
    }

    std::span<const std::byte> packet_;
    size_t offset_ = 0;
};

// Opcodes LMS sends that this client intentionally ignores.
constexpr std::array<std::string_view, 9> kIgnoredOps{"DBUG", "SYST", "visu", "IR  ", "GRFe",
                                                      "GRFh", "GRFb", "GRFm", "OCOB"};

}  // namespace

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
    default: return 2;  // '2' and anything unknown: stereo
    }
}

PcmFormat pcmFormat(const PcmParams& params, uint32_t fallbackRate) {
    PcmFormat f;
    if (params.sampleSizeCode != '?')
        f.bitsPerSample = bitsPerSampleFromCode(params.sampleSizeCode);
    if (params.sampleRateCode != '?')
        f.sampleRate = sampleRateFromCode(params.sampleRateCode);
    else
        f.sampleRate = fallbackRate;
    if (params.channelsCode != '?') f.channels = channelsFromCode(params.channelsCode);
    if (params.endianCode != '?') f.bigEndian = (params.endianCode == '0');
    return f;
}

void SlimProtoClient::sendHelo(bool reconnect) {
    const std::string& caps = caps_;
    PacketWriter body(kHeloBodyBytes + caps.size());
    body.u8(kHeloDeviceId);  // deviceid 12 = squeezeplay class (squeezelite parity)
    body.u8(kHeloRevision);  // single byte, shown as player firmware rev
    body.bytes(std::as_bytes(std::span{mac_}));
    body.skip(16);  // reserved: packet bytes 8..23
    body.u16(static_cast<uint16_t>(reconnect ? kHeloFlagReconnect : 0x0000));
    body.skip(10);  // reserved: packet bytes 26..35
    body.bytes(std::as_bytes(std::span{caps}));

    log::info(log::Area::Lms, "HELO mac={} cap={}", macToString(mac_), caps);
    if (!sendPacket(kOpHelo, body.data())) log::error(log::Area::Lms, "HELO send failed");
}

StreamStats SlimProtoClient::lastStats() {
    std::lock_guard<std::mutex> lock(sendMutex_);
    return stats_;
}

void SlimProtoClient::sendStat(const char (&event)[5], StreamStats stats,
                               uint32_t serverTimestamp) {
    {
        // stats_ is read back by the run thread ('f'/'p'/'u' handlers) and
        // written from stream threads; sendMutex_ serializes both.
        std::lock_guard<std::mutex> lock(sendMutex_);
        stats_ = stats;
    }
    PacketWriter body(kStatBodyBytes);
    body.bytes(std::as_bytes(std::span{event}.first(4)));
    body.skip(3);  // reserved: packet bytes 4..6 (the old code zeroed byte 6)
    body.u32(stats.streamBufferSize);
    body.u32(stats.streamBufferFullness);
    body.u32(static_cast<uint32_t>(stats.bytesReceived >> 32));
    body.u32(static_cast<uint32_t>(stats.bytesReceived & 0xFFFFFFFFULL));
    body.u16(0xFFFF);
    body.u32(static_cast<uint32_t>(nowMs()));
    body.u32(stats.outputBufferSize);
    body.u32(stats.outputBufferFullness);
    body.u32(stats.elapsedMs / 1000);
    body.u16(0);
    body.u32(stats.elapsedMs);
    body.u32(serverTimestamp);
    body.u16(0);
    if (!sendPacket(kOpStat, body.data())) log::warn(log::Area::Lms, "STAT send failed");
}

void SlimProtoClient::sendResp(const std::string& header) {
    if (!sendPacket(kOpResp, std::as_bytes(std::span{header})))
        log::warn(log::Area::Lms, "RESP send failed");
}

void SlimProtoClient::sendSetdName(const std::string& name) {
    PacketWriter body(kSetdBodyBytes + name.size());
    body.u8(0);
    body.bytes(std::as_bytes(std::span{name}));
    body.u8(0);  // trailing NUL
    if (!sendPacket(kOpSetd, body.data())) log::warn(log::Area::Lms, "SETD send failed");
}

void SlimProtoClient::sendDisco(uint8_t reason) {
    if (!sendPacket(kOpDsco, std::as_bytes(std::span{&reason, 1})))
        log::warn(log::Area::Lms, "DSCO send failed");
}

void SlimProtoClient::sendButton(uint32_t code) {
    // BUTN: [4b time in 1 kHz ticks][4b button code]. LMS's _button_handler
    // unpacks NH8 and resolves the code against the player's IR code set
    // (IR/Slim_Devices_Remote.ir: volup=7689807f, voldown=768900ff), then runs
    // the mapped button function (Default.map: volup/voldown -> volume).
    // Timestamps must strictly increase: LMS ignores a duplicate.
    uint32_t tick = static_cast<uint32_t>(nowMs() & 0xFFFFFFFFULL);
    uint32_t prev = buttonTick_.load(std::memory_order_relaxed);
    uint32_t next = 0;
    do {
        next = tick > prev ? tick : prev + 1u;
    } while (!buttonTick_.compare_exchange_weak(prev, next, std::memory_order_relaxed));

    PacketWriter body(kButnBodyBytes);
    body.u32(next);
    body.u32(code);
    if (!sendPacket(kOpButn, body.data())) log::warn(log::Area::Lms, "BUTN send failed");
}

void SlimProtoClient::sendMeta(std::string_view data) {
    // squeezelite parity: forward the raw ICY metadata block to LMS so its
    // track display follows the stream (LMS also watches direct streams
    // itself; this is redundant there but keeps proxied streams in sync).
    // The ICY de-interleaver only invokes this with a non-empty block; an
    // empty block carries no information for LMS either way.
    if (data.empty()) return;
    (void)sendPacket(kOpMeta, std::as_bytes(std::span{data}));  // best effort:
    // metadata is cosmetic; connection health is the read loop's job
}

void SlimProtoClient::process(const std::string& pkt) {
    const size_t len = pkt.size();
    if (len < kMinPacketBytes) return;
    const std::string_view op(pkt.data(), kOpcodeBytes);
    PacketReader r(std::as_bytes(std::span{pkt}));
    if (!r.skip(kOpcodeBytes)) return;  // opcode (len bound already guarantees this)

    if (op == kOpStrm) {
        if (len < kStrmMinBytes) return;
        const uint8_t command = *r.u8();
        switch (command) {
        case kStrmHeartbeat: {
            // heartbeat timestamp sits at packet bytes 18..21
            const auto ts = r.u32At(18);
            if (!ts) return;
            // Reply with the real stats (squeezelite parity): a zeroed reply
            // makes LMS's progress display drop to 0 until the next heartbeat
            // and clobbers the cached lastStats() used by later replies.
            sendStat(kStatHeartbeat, statsProvider_ ? statsProvider_() : StreamStats{}, *ts);
            lastHeartbeatMs_ = nowMs();
            break;
        }
        case kStrmStop:
            log::debug(log::Area::Lms, "strm q (stop)");
            if (events_.onStop) events_.onStop();
            break;
        case kStrmFlush:
            log::debug(log::Area::Lms, "strm f (flush)");
            // The STMf ack is emitted by the onFlush handler (PlayerSession),
            // matching the 'q' stop path; sending it here as well duplicated
            // the STAT packet.
            if (events_.onFlush) events_.onFlush(true);
            break;
        case kStrmPause: {
            const auto ms = r.u32At(18);
            if (!ms) return;
            log::debug(log::Area::Lms, "strm p (pause, interval={})", *ms);
            if (events_.onPause) events_.onPause(*ms);
            if (!*ms) sendStat(kStatPause, lastStats());
            break;
        }
        case kStrmSkip: {
            const auto ms = r.u32At(18);
            if (!ms) return;
            log::debug(log::Area::Lms, "strm a (skip ahead, interval={})", *ms);
            if (events_.onSkipAhead) events_.onSkipAhead(*ms);
            break;
        }
        case kStrmUnpause: {
            const auto jiffies = r.u32At(18);
            if (!jiffies) return;
            log::debug(log::Area::Lms, "strm u (unpause, jiffies={})", *jiffies);
            if (events_.onUnpause) events_.onUnpause(*jiffies);
            sendStat(kStatResume, lastStats());
            break;
        }
        case kStrmStart: {
            if (len < kStrmStartMinBytes) return;
            StrmStart st;
            st.autostart = static_cast<uint8_t>(*r.u8() - '0');
            st.format = static_cast<StreamFormat>(*r.u8());
            st.pcm.sampleSizeCode = *r.u8();
            st.pcm.sampleRateCode = *r.u8();
            st.pcm.channelsCode = *r.u8();
            st.pcm.endianCode = *r.u8();
            st.thresholdKb = *r.u8();
            if (!r.skip(1)) return;  // packet byte 12 (unused)
            st.transitionPeriodS = *r.u8();
            st.transitionType = static_cast<uint8_t>(*r.u8() - '0');
            st.flags = *r.u8();  // packet byte 15; 0x20 = TLS on a direct URL
            st.ssl = (st.flags & kStrmFlagSsl) != 0;
            if (!r.skip(1)) return;  // packet byte 16 (output threshold)
            if (!r.skip(1)) return;  // packet byte 17 (slaves)
            st.replayGain = *r.u32();
            st.serverPort = *r.u16();
            st.serverIp = *r.u32();
            const auto rest = r.tail();
            st.request.assign(reinterpret_cast<const char*>(rest.data()), rest.size());
            log::debug(log::Area::Lms, "strm s autostart={} format={} threshold={}", st.autostart,
                       static_cast<char>(st.format), st.thresholdKb);
            sendStat(kStatFlush, lastStats());
            if (events_.onStart) events_.onStart(st);
            break;
        }
        default: log::warn(log::Area::Lms, "unhandled strm command '{}'", command); break;
        }
    } else if (op == kOpCont) {
        if (const auto metaint = r.u32()) {
            if (events_.onCont) events_.onCont(*metaint);
        }
    } else if (op == kOpCodc) {
        if (len < kCodcMinBytes) return;  // opcode + format + 4 pcm bytes
        const StreamFormat f = static_cast<StreamFormat>(*r.u8());
        const PcmParams pcm{*r.u8(), *r.u8(), *r.u8(), *r.u8()};
        if (events_.onCodc) events_.onCodc(f, pcm);
    } else if (op == kOpAudg) {
        if (len < kAudgMinBytes) return;
        if (!r.skip(8)) return;  // packet bytes 4..11
        const uint8_t adjust = *r.u8();
        // dvc=0 is LMS's fixed-output mode: the gains are the no-op 1.0 and
        // applying them would push 0 dB = full blast. Leave the receiver at
        // its current level.
        if (!adjust) {
            log::debug(log::Area::Lms, "audg dvc=0 ignored (fixed-output mode)");
            return;
        }
        if (!r.skip(1)) return;  // packet byte 13
        // new_left/new_right are 16.16 fixed-point linear amplitude
        // multipliers (1.0 = full volume). Recover the LMS slider percent
        // with the curve for OUR player class (deviceid 12 = SqueezePlay,
        // the Boom curve), not Squeezebox2's single ramp; see volume_map.h.
        // The bridge passes the percent straight to the AirPlay sender's
        // 0..100 % domain, whose 0 % is the -144 mute sentinel and 100 % is
        // 0 dB.
        const uint32_t gainL = *r.u32();
        const uint32_t gainR = *r.u32();
        if (events_.onVolume)
            events_.onVolume(lmsSliderPctFromGain(gainL), lmsSliderPctFromGain(gainR));
    } else if (op == kOpAude) {
        // Output enable/disable. squeezelite keys power on enable_spdif and
        // ignores enable_dac; mirror that.
        if (len < kAudeMinBytes) return;
        const uint8_t enableSpdif = *r.u8();
        if (!r.skip(1)) return;  // enable_dac
        if (events_.onAude) events_.onAude(enableSpdif != 0);
    } else if (op == kOpSetdServer) {
        if (len < kSetdMinBytes) return;
        if (*r.u8() != 0) return;
        if (len == kSetdMinBytes) {
            sendSetdName(playerName_.empty() ? "squeeze2raop2" : playerName_);
        } else {
            std::string name(reinterpret_cast<const char*>(r.tail().data()), r.remaining());
            while (!name.empty() && name.back() == '\0') name.pop_back();
            // Remember the name so a later 5-byte query echoes the new one.
            playerName_ = name;
            if (events_.onSetName) events_.onSetName(name);
            sendSetdName(name);
        }
    } else if (op == kOpServ) {
        if (len < kServMinBytes) return;
        const uint32_t ip = *r.u32();
        if (events_.onServerSwitch) events_.onServerSwitch(ip);
    } else if (std::ranges::find(kIgnoredOps, op) != kIgnoredOps.end()) {
        log::debug(log::Area::Lms, "ignored {}", op);
    } else {
        log::warn(log::Area::Lms, "unhandled opcode {}", op);
    }
}

}  // namespace squeeze2raop2