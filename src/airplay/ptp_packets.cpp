#include "airplay/ptp_packets.h"

#include "common/byte_order.h"

namespace squeeze2raop2::ptp {

namespace {

// PTP fixed-width fields are big-endian. The byte swap/convert is the shared
// common/byte_order.h helper (used by slimproto, the MP4 demux, ...); here the
// wire buffer is a std::string, the vendored sender's convention.
void putU16(std::string& out, uint16_t v) {
    std::byte b[2];
    writeInt<Endian::Big>(b, v);
    out.append(reinterpret_cast<const char*>(b), 2);
}
void putU32(std::string& out, uint32_t v) {
    std::byte b[4];
    writeInt<Endian::Big>(b, v);
    out.append(reinterpret_cast<const char*>(b), 4);
}
void putU64(std::string& out, uint64_t v) {
    std::byte b[8];
    writeInt<Endian::Big>(b, v);
    out.append(reinterpret_cast<const char*>(b), 8);
}
uint16_t getU16(std::span<const uint8_t> d, size_t off) {
    return readInt<Endian::Big, uint16_t>(reinterpret_cast<const std::byte*>(d.data()) + off);
}
uint32_t getU32(std::span<const uint8_t> d, size_t off) {
    return readInt<Endian::Big, uint32_t>(reinterpret_cast<const std::byte*>(d.data()) + off);
}
uint64_t getU64(std::span<const uint8_t> d, size_t off) {
    return readInt<Endian::Big, uint64_t>(reinterpret_cast<const std::byte*>(d.data()) + off);
}

constexpr uint8_t kGptpOrg[3] = {0x00, 0x80, 0xC2};
constexpr uint8_t kAppleOrg[3] = {0x00, 0x0D, 0x93};
constexpr uint8_t kSubFollowUpInfo[3] = {0x00, 0x00, 0x01};
constexpr uint8_t kSubIntervalRequest[3] = {0x00, 0x00, 0x02};

// Apple proprietary TLV payloads in the Mac sender's form (0x2710 markers).
constexpr uint8_t kAppleSub01[3] = {0x00, 0x00, 0x01};
constexpr uint8_t kAppleSub05[3] = {0x00, 0x00, 0x05};
constexpr uint8_t kApple01Mac[16] = {0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x27, 0x10,
                                     0x00, 0x00, 0x27, 0x10, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t kApple05Mac[26] = {0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x27, 0x10, 0x00,
                                     0x00, 0x27, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Organization-extension TLV: type 0x0003, length 6 + payload, then org +
// subtype + payload.
void putOrgTlv(std::string& out, const uint8_t org[3], const uint8_t sub[3],
               const std::string& payload) {
    putU16(out, 0x0003);
    putU16(out, static_cast<uint16_t>(6 + payload.size()));
    out.append(reinterpret_cast<const char*>(org), 3);
    out.append(reinterpret_cast<const char*>(sub), 3);
    out += payload;
}

// gPTP message-interval-request payload: linkDelay, timeSync, announce, flags.
std::string intervalPayload(int8_t sync, int8_t announce, uint8_t flags) {
    std::string p;
    p.reserve(4);
    p.push_back(static_cast<char>(static_cast<uint8_t>(sync)));
    p.push_back(static_cast<char>(static_cast<uint8_t>(sync)));
    p.push_back(static_cast<char>(static_cast<uint8_t>(announce)));
    p.push_back(static_cast<char>(flags));
    return p;
}

std::string signalingWith(const std::string& tlvs, uint64_t clockId, uint16_t sequence) {
    Header h;
    h.type = MessageType::Signaling;
    h.messageLength = static_cast<uint16_t>(kHeaderSize + 10 + tlvs.size());
    h.flags = kAppleFlags;
    h.sourceClockIdentity = clockId;
    h.sourcePort = 1;
    h.sequenceId = sequence;
    h.controlField = 5;
    h.logMessageInterval = 127;
    std::string out = encodeHeader(h);
    out.append(10, static_cast<char>(0xFF));  // targetPortIdentity: wildcard
    out += tlvs;
    return out;
}

}  // namespace

std::string encodeTimestamp(uint64_t nanoseconds) {
    const uint64_t seconds = nanoseconds / 1000000000ULL;
    const uint32_t nanos = static_cast<uint32_t>(nanoseconds % 1000000000ULL);
    std::string out;
    out.reserve(kTimestampSize);
    for (int shift = 40; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((seconds >> shift) & 0xFF));  // 48-bit seconds
    putU32(out, nanos);
    return out;
}

std::optional<uint64_t> parseTimestamp(std::span<const uint8_t> data) {
    if (data.size() < kTimestampSize) return std::nullopt;
    uint64_t seconds = 0;
    for (size_t i = 0; i < 6; ++i) seconds = (seconds << 8) | data[i];
    const uint32_t nanos = getU32(data, 6);
    return seconds * 1000000000ULL + nanos;
}

std::string encodeHeader(const Header& header) {
    std::string out;
    out.reserve(kHeaderSize);
    out.push_back(static_cast<char>(((header.transportSpecific & 0x0F) << 4) |
                                    (static_cast<uint8_t>(header.type) & 0x0F)));
    out.push_back(static_cast<char>(header.version & 0x0F));
    putU16(out, header.messageLength);
    out.push_back(static_cast<char>(header.domainNumber));
    out.push_back('\0');
    putU16(out, header.flags);
    putU64(out, static_cast<uint64_t>(header.correctionField));
    putU32(out, 0);  // reserved
    putU64(out, header.sourceClockIdentity);
    putU16(out, header.sourcePort);
    putU16(out, header.sequenceId);
    out.push_back(static_cast<char>(header.controlField));
    out.push_back(static_cast<char>(static_cast<uint8_t>(header.logMessageInterval)));
    return out;
}

std::optional<Header> parseHeader(std::span<const uint8_t> data) {
    if (data.size() < kHeaderSize) return std::nullopt;
    Header header;
    switch (data[0] & 0x0F) {
    case 0x00: header.type = MessageType::Sync; break;
    case 0x01: header.type = MessageType::DelayReq; break;
    case 0x08: header.type = MessageType::FollowUp; break;
    case 0x09: header.type = MessageType::DelayResp; break;
    case 0x0B: header.type = MessageType::Announce; break;
    case 0x0C: header.type = MessageType::Signaling; break;
    default: return std::nullopt;
    }
    header.version = data[1] & 0x0F;
    header.messageLength = getU16(data, 2);
    header.domainNumber = data[4];
    header.flags = getU16(data, 6);
    header.correctionField = static_cast<int64_t>(getU64(data, 8));
    header.sourceClockIdentity = getU64(data, 20);
    header.sourcePort = getU16(data, 28);
    header.sequenceId = getU16(data, 30);
    header.controlField = data[32];
    header.logMessageInterval = static_cast<int8_t>(data[33]);
    return header;
}

std::string announce(uint64_t clockId, uint16_t sequence, uint8_t priority1, uint8_t clockClass) {
    Header h;
    h.type = MessageType::Announce;
    h.messageLength = 64;
    h.flags = kAppleFlags;
    h.sourceClockIdentity = clockId;
    h.sourcePort = 1;
    h.sequenceId = sequence;
    h.controlField = 5;
    h.logMessageInterval = 254;
    std::string out = encodeHeader(h);
    out.append(10, '\0');  // originTimestamp (zero for Announce)
    putU16(out, 37);       // currentUtcOffset (TAI - UTC)
    out.push_back('\0');   // reserved
    out.push_back(static_cast<char>(priority1));
    out.push_back(static_cast<char>(clockClass));
    out.push_back(static_cast<char>(0xFE));  // clockAccuracy: unknown
    putU16(out, 0xFFFF);                     // offsetScaledLogVariance: unknown
    out.push_back(static_cast<char>(128));   // priority2
    putU64(out, clockId);                    // grandmasterIdentity
    putU16(out, 0);                          // stepsRemoved
    out.push_back(static_cast<char>(0xA0));  // timeSource: internal oscillator
    return out;
}

std::string macSignaling(uint64_t clockId, uint16_t sequence) {
    std::string tlvs;
    putOrgTlv(tlvs, kGptpOrg, kSubIntervalRequest, intervalPayload(-3, -2, 0x02));
    putOrgTlv(tlvs, kAppleOrg, kAppleSub01,
              std::string(reinterpret_cast<const char*>(kApple01Mac), sizeof(kApple01Mac)));
    putOrgTlv(tlvs, kAppleOrg, kAppleSub05,
              std::string(reinterpret_cast<const char*>(kApple05Mac), sizeof(kApple05Mac)));
    return signalingWith(tlvs, clockId, sequence);
}

std::string stopSignaling(uint64_t clockId, uint16_t sequence) {
    std::string tlvs;
    putOrgTlv(tlvs, kGptpOrg, kSubIntervalRequest, intervalPayload(0x7E, 0x7E, 0x00));
    return signalingWith(tlvs, clockId, sequence);
}

std::pair<std::string, std::string> syncFollowUp(uint64_t clockId, uint16_t sequence,
                                                 uint64_t sendTimeNs) {
    Header sh;
    sh.type = MessageType::Sync;
    sh.messageLength = kHeaderSize + kTimestampSize;
    sh.flags = kTwoStepFlag | kAppleFlags;
    sh.sourceClockIdentity = clockId;
    sh.sourcePort = 1;
    sh.sequenceId = sequence;
    sh.controlField = 0;
    sh.logMessageInterval = 253;
    std::string sync = encodeHeader(sh);
    sync.append(kTimestampSize, '\0');  // two-step: real time rides the Follow_Up

    // Follow_Up Information TLV payload after org+subtype: rate offset (4),
    // GM time base indicator (2), last GM phase change (12), freq change (4).
    std::string tlv;
    putOrgTlv(tlv, kGptpOrg, kSubFollowUpInfo, std::string(22, '\0'));

    Header fh;
    fh.type = MessageType::FollowUp;
    fh.messageLength = static_cast<uint16_t>(kHeaderSize + kTimestampSize + tlv.size());
    fh.flags = kAppleFlags;
    fh.sourceClockIdentity = clockId;
    fh.sourcePort = 1;
    fh.sequenceId = sequence;
    fh.controlField = 2;
    fh.logMessageInterval = 253;
    std::string follow = encodeHeader(fh);
    follow += encodeTimestamp(sendTimeNs);
    follow += tlv;
    return {sync, follow};
}

std::string delayRequest(uint64_t clockId, uint16_t sourcePort, uint16_t sequence,
                         uint64_t sendTimeNs) {
    Header h;
    h.type = MessageType::DelayReq;
    h.messageLength = kHeaderSize + kTimestampSize;
    h.flags = kAppleFlags;
    h.sourceClockIdentity = clockId;
    h.sourcePort = sourcePort;
    h.sequenceId = sequence;
    h.controlField = 0;
    h.logMessageInterval = 253;
    std::string out = encodeHeader(h);
    out += encodeTimestamp(sendTimeNs);
    return out;
}

std::string buildSyncPacket(uint16_t sequence, uint32_t currentRtp, uint64_t receiverTimeNs,
                            uint32_t nextRtp, uint64_t masterClockId, bool first) {
    std::string out;
    out.reserve(28);
    out.push_back(static_cast<char>(first ? 0x90 : 0x80));
    out.push_back(static_cast<char>(0xD7));  // marker | PT 87
    putU16(out, sequence);
    putU32(out, currentRtp);
    putU64(out, receiverTimeNs);  // 64-bit nanoseconds (Apple/owntone format)
    putU32(out, nextRtp);
    putU64(out, masterClockId);
    return out;
}

std::optional<AnnounceInfo> parseAnnounce(std::span<const uint8_t> data) {
    if (data.size() < 61) return std::nullopt;
    AnnounceInfo info;
    info.priority1 = data[47];
    info.grandmasterClockId = getU64(data, 53);
    return info;
}

std::optional<uint64_t> parseBodyTimestamp(std::span<const uint8_t> data) {
    if (data.size() < kHeaderSize + kTimestampSize) return std::nullopt;
    return parseTimestamp(data.subspan(kHeaderSize, kTimestampSize));
}

}  // namespace squeeze2raop2::ptp
