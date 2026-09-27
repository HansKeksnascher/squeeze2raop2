#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

// IEEE 1588-2008 / 802.1AS (gPTP) wire format for the AirPlay 2 timing path.
// Pure functions over byte buffers -- no sockets, clock or threads -- so the
// packet layout is unit-testable on its own. PtpClock drives them.
namespace squeeze2raop2::ptp {

// Standard gPTP transport ports: event carries Sync/Delay_Req, general carries
// Announce/Follow_Up/Delay_Resp/Signaling.
constexpr uint16_t kEventPort = 319;
constexpr uint16_t kGeneralPort = 320;
constexpr size_t kHeaderSize = 34;
constexpr size_t kTimestampSize = 10;
constexpr uint8_t kVersion = 2;
// Apple's AirPlay gPTP profile (as seen from a real iPhone->Sonos capture) sets
// transportSpecific = 1 on every message and raises the unicast + profile flag
// bits. Receivers drop packets that do not carry transportSpecific = 1, which is
// why a Delay_Req sent with the default 0 is never answered.
constexpr uint8_t kTransportSpecific = 1;
constexpr uint16_t kUnicastFlag = 0x0400;
constexpr uint16_t kTwoStepFlag = 0x0200;
constexpr uint16_t kProfileFlag = 0x0008;
constexpr uint16_t kAppleFlags = kUnicastFlag | kProfileFlag;

enum class MessageType : uint8_t {
    Sync = 0x00,
    DelayReq = 0x01,
    FollowUp = 0x08,
    DelayResp = 0x09,
    Announce = 0x0B,
    Signaling = 0x0C,
};

struct Header {
    MessageType type = MessageType::Sync;
    uint8_t transportSpecific = kTransportSpecific;
    uint8_t version = kVersion;
    uint16_t messageLength = kHeaderSize + kTimestampSize;
    uint8_t domainNumber = 0;
    uint16_t flags = 0;
    int64_t correctionField = 0;
    uint64_t sourceClockIdentity = 0;
    uint16_t sourcePort = 1;
    uint16_t sequenceId = 0;
    uint8_t controlField = 0;
    int8_t logMessageInterval = 0;
};

// PTP timestamps are 48-bit seconds + 32-bit nanoseconds (10 bytes, BE).
std::string encodeTimestamp(uint64_t nanoseconds);
std::optional<uint64_t> parseTimestamp(std::span<const uint8_t> data);

std::string encodeHeader(const Header& header);
std::optional<Header> parseHeader(std::span<const uint8_t> data);

// Builders. `clockId` is our 8-byte gPTP identity, `sequence` our counter.
std::string announce(uint64_t clockId, uint16_t sequence, uint8_t priority1, uint8_t clockClass);
// Mac-style Signaling: gPTP interval-request + the two Apple TLVs a Mac sends.
std::string macSignaling(uint64_t clockId, uint16_t sequence);
// "Stop" Signaling (all intervals 0x7E) sent when yielding the master role.
std::string stopSignaling(uint64_t clockId, uint16_t sequence);
// Two-step Sync (event port) + Follow_Up (general port) pair. `sendTimeNs` is
// the Sync transmit time, carried in the Follow_Up.
std::pair<std::string, std::string> syncFollowUp(uint64_t clockId, uint16_t sequence,
                                                 uint64_t sendTimeNs);
// Delay_Req with OUR source identity: the Sonos/third-party receivers reply
// with a Delay_Resp to this form (the iPhone does the same). HomePods ignore
// Delay_Req either way, so this only adds the two-way exchange where it works.
std::string delayRequest(uint64_t clockId, uint16_t sourcePort, uint16_t sequence,
                         uint64_t sendTimeNs);

// AirPlay 2 PTP-mode RTP control sync packet (payload type 0xD7, 28 bytes). It
// maps an RTP timestamp onto the receiver's own PTP clock and names the master
// clock. The clock field is a 64-bit big-endian nanosecond count (verified
// against a real iPhone->Sonos capture), not seconds+32-bit-fraction.
std::string buildSyncPacket(uint16_t sequence, uint32_t currentRtp, uint64_t receiverTimeNs,
                            uint32_t nextRtp, uint64_t masterClockId, bool first);

// Parsed views of the messages a slave consumes.
struct AnnounceInfo {
    uint8_t priority1 = 255;
    uint64_t grandmasterClockId = 0;
};
std::optional<AnnounceInfo> parseAnnounce(std::span<const uint8_t> data);
// The t1 timestamp from a Follow_Up or Delay_Resp body (bytes 34..43).
std::optional<uint64_t> parseBodyTimestamp(std::span<const uint8_t> data);

}  // namespace squeeze2raop2::ptp
