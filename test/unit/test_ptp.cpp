// PTP timing: pure packet layout (IEEE 1588 / 802.1AS) and the shared clock's
// one-way Sync/Follow_Up convergence. The loopback case drives a real
// PtpClock over ephemeral ports with a hand-built master, no root needed.

#include "airplay/ptp_clock.h"
#include "airplay/ptp_packets.h"

#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <thread>

using namespace squeeze2raop2;
using namespace squeeze2raop2::test;

namespace {

std::span<const uint8_t> asBytes(const std::string& s) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
uint8_t byteAt(const std::string& s, size_t i) { return static_cast<uint8_t>(s[i]); }
uint16_t be16At(const std::string& s, size_t i) {
    return static_cast<uint16_t>((static_cast<uint16_t>(byteAt(s, i)) << 8) | byteAt(s, i + 1));
}
uint32_t be32At(const std::string& s, size_t i) {
    return (static_cast<uint32_t>(byteAt(s, i)) << 24) |
           (static_cast<uint32_t>(byteAt(s, i + 1)) << 16) |
           (static_cast<uint32_t>(byteAt(s, i + 2)) << 8) | static_cast<uint32_t>(byteAt(s, i + 3));
}

int makeUdp() { return ::socket(AF_INET, SOCK_DGRAM, 0); }

void sendUdp(int fd, uint16_t port, const std::string& data) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    (void)::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    (void)::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

}  // namespace

SQ2_TEST(ptp, timestamp_roundtrip) {
    const uint64_t ns = 1234567890123456789ULL;
    const std::string enc = ptp::encodeTimestamp(ns);
    expect(enc.size() == ptp::kTimestampSize, "timestamp is 10 bytes");
    const auto back = ptp::parseTimestamp(asBytes(enc));
    require(back.has_value(), "timestamp parses");
    expect(*back == ns, "timestamp round-trips");
}

SQ2_TEST(ptp, header_roundtrip) {
    ptp::Header h;
    h.type = ptp::MessageType::FollowUp;
    h.messageLength = 76;
    h.flags = ptp::kTwoStepFlag;
    h.sourceClockIdentity = 0x1122334455667788ULL;
    h.sourcePort = 1;
    h.sequenceId = 42;
    h.controlField = 2;
    h.logMessageInterval = -3;
    const std::string enc = ptp::encodeHeader(h);
    expect(enc.size() == ptp::kHeaderSize, "header is 34 bytes");
    const auto back = ptp::parseHeader(asBytes(enc));
    require(back.has_value(), "header parses");
    expect(back->type == ptp::MessageType::FollowUp, "type");
    expect(back->messageLength == 76, "message length");
    expect(back->flags == ptp::kTwoStepFlag, "two-step flag");
    expect(back->sourceClockIdentity == h.sourceClockIdentity, "clock identity");
    expect(back->sequenceId == 42, "sequence");
    expect(back->logMessageInterval == -3, "log message interval");
}

SQ2_TEST(ptp, announce_fields) {
    const uint64_t id = 0xAABBCCDDEEFF0011ULL;
    const std::string pkt = ptp::announce(id, 7, 248, 248);
    expect(pkt.size() == 64, "announce is 64 bytes");
    const auto info = ptp::parseAnnounce(asBytes(pkt));
    require(info.has_value(), "announce parses");
    expect(info->priority1 == 248, "priority1");
    expect(info->grandmasterClockId == id, "grandmaster clock identity");
}

SQ2_TEST(ptp, sync_follow_up_carries_t1) {
    const uint64_t id = 0x1122334455667788ULL;
    const uint64_t t1 = 987654321000000000ULL;  // 987654321 s
    const auto pair = ptp::syncFollowUp(id, 3, t1);
    const std::string& sync = pair.first;
    const std::string& follow = pair.second;
    expect(sync.size() == 44, "sync is 44 bytes");
    expect((byteAt(sync, 0) & 0x0F) == 0x00, "sync message type");
    expect((byteAt(sync, 0) >> 4) == ptp::kTransportSpecific, "sync transportSpecific");
    expect(be16At(sync, 6) == (ptp::kTwoStepFlag | ptp::kAppleFlags), "sync flags");
    expect((byteAt(follow, 0) & 0x0F) == 0x08, "follow_up message type");
    const auto parsed = ptp::parseBodyTimestamp(asBytes(follow));
    require(parsed.has_value(), "follow_up t1 parses");
    expect(*parsed == t1, "t1 value");
}

SQ2_TEST(ptp, delay_request_uses_apple_profile) {
    // Apple (and the Sonos) only answer a Delay_Req carrying transportSpecific
    // = 1 and the unicast + profile flags; with the default 0 the Sonos stays
    // silent (no Delay_Resp), which is what our bridge capture showed.
    const std::string pkt = ptp::delayRequest(0x1122334455667788ULL, 1, 9, 123456789ULL);
    expect(pkt.size() == 44, "delay_req is 44 bytes");
    expect((byteAt(pkt, 0) >> 4) == ptp::kTransportSpecific, "transportSpecific=1");
    expect((byteAt(pkt, 0) & 0x0F) == 0x01, "delay_req message type");
    expect(be16At(pkt, 6) == ptp::kAppleFlags, "unicast+profile flags");
    expect(byteAt(pkt, 32) == 0, "control field 0");
    expect(byteAt(pkt, 33) == 253, "log message interval 253");
}

SQ2_TEST(ptp, sync_packet_layout) {
    const uint64_t master = 0x0102030405060708ULL;
    // 1.5 s in nanoseconds -> 64-bit big-endian 0x0000000059682F00.
    const std::string pkt = ptp::buildSyncPacket(5, 66150, 1500000000ULL, 66502, master, true);
    require(pkt.size() == 28, "ptp sync is 28 bytes");
    expect(byteAt(pkt, 0) == 0x90, "first lead byte");
    expect(byteAt(pkt, 1) == 0xD7, "payload type 0xD7");
    expect(be16At(pkt, 2) == 5, "sequence");
    expect(be32At(pkt, 4) == 66150, "current rtp");
    expect(be32At(pkt, 8) == 0, "ptp ns high word");
    expect(be32At(pkt, 12) == 0x59682F00u, "ptp ns low word");
    expect(be32At(pkt, 16) == 66502, "next rtp");
    expect(pkt.substr(20, 8) == std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8),
           "master clock id");
    const std::string later = ptp::buildSyncPacket(6, 66502, 0, 66854, master, false);
    expect(byteAt(later, 0) == 0x80, "subsequent lead byte");
}

SQ2_TEST(ptp, clock_converges_on_sync_follow_up) {
    PtpClock::Config cfg;
    cfg.eventPort = 0;  // ephemeral: no privileged port / nqptp conflict
    cfg.generalPort = 0;
    cfg.joinMulticast = false;
    // Short windows so the grandmaster-fallback path is exercised here.
    cfg.probeTimeout = std::chrono::milliseconds(80);
    cfg.masterInterval = std::chrono::milliseconds(50);
    PtpClock clock;
    require(clock.start(cfg), "PtpClock start");
    require(clock.available(), "PtpClock available");
    const uint16_t ep = clock.eventPort();
    const uint16_t gp = clock.generalPort();
    require(ep != 0 && gp != 0, "bound ports known");

    clock.addPeer("127.0.0.1");  // BMCA burst goes to the standard ports, ignored

    const int fd = makeUdp();
    require(fd >= 0, "test socket");
    const uint64_t masterId = 0xCAFEBABE12345678ULL;
    const uint64_t t1 = 5000000000ULL;  // 5 s on the receiver's own clock
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    bool synced = false;
    for (int i = 0; i < 8 && !synced; ++i) {
        const auto pair = ptp::syncFollowUp(masterId, static_cast<uint16_t>(i + 1), t1);
        sendUdp(fd, ep, pair.first);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        sendUdp(fd, gp, pair.second);
        for (int w = 0; w < 5 && !synced; ++w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            synced = clock.synchronized("127.0.0.1");
        }
    }
    expect(synced, "PTP clock synchronized from Sync/Follow_Up");
    if (synced) {
        expect(clock.peerClockId("127.0.0.1") == masterId, "master clock id learned");
        const uint64_t now = clock.nowNs("127.0.0.1");
        expect(now >= t1 && now < t1 + 1000000000ULL, "receiver time tracks t1");
        const auto sample = clock.sample("127.0.0.1");
        require(sample.has_value(), "sample present");
        expect(sample->deviceTimeNs == t1, "sample device time");
    }
    ::close(fd);
    clock.stop();
}
