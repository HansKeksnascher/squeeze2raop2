#include "slimproto.h"
#include "util.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <string>

using namespace squeeze2raop2;

static void testPackN() {
    uint8_t buf[8] = {};
    packN(std::as_writable_bytes(std::span{buf}), 0x0102030405060708ULL, 8);
    if (buf[0] != 0x01 || buf[1] != 0x02 || buf[6] != 0x07 || buf[7] != 0x08) {
        fprintf(stderr, "packN byte order wrong\n");
        exit(1);
    }
    packN(std::as_writable_bytes(std::span{buf}).subspan(0, 2), 0x1234, 2);
    if (buf[0] != 0x12 || buf[1] != 0x34) exit(1);
    if (unpackN(std::as_bytes(std::span{buf}).subspan(0, 2)) != 0x1234) exit(1);
    // span clamping: oversized count must not write past the span
    uint8_t small[2] = {};
    packN(std::as_writable_bytes(std::span{small}), 0xFFFFFFFFu, 8);
    if (small[0] != 0xFF || small[1] != 0xFF) exit(1);
}

static void testPcmCodes() {
    if (sampleRateFromCode('3') != 44100) exit(1);
    if (sampleRateFromCode('9') != 96000) exit(1);
    if (bitsPerSampleFromCode('1') != 16) exit(1);
    if (channelsFromCode('2') != 2) exit(1);
    PcmParams p{'1', '3', '2', '0'};
    PcmFormat f = pcmFormat(p, 48000);
    if (f.bitsPerSample != 16 || f.sampleRate != 44100 || f.channels != 2 || !f.bigEndian) exit(1);
    PcmParams unknown{'?', '?', '?', '?'};
    PcmFormat d = pcmFormat(unknown, 48000);
    if (d.sampleRate != 48000 || d.bitsPerSample != 16 || d.channels != 2 || d.bigEndian) exit(1);
}

static void testMac() {
    auto m = fakeMacFor("kitchen");
    if (m[0] != 0xaa) exit(1);
    auto m2 = fakeMacFor("kitchen");
    if (m != m2) exit(1);
    auto other = fakeMacFor("living room");
    if (other == m) exit(1);
    std::array<uint8_t, 6> parsed{};
    if (!macFromString("aa:01:02:03:04:05", parsed)) exit(1);
    if (parsed[0] != 0xaa || parsed[5] != 0x05) exit(1);
    if (macFromString("zz:01:02:03:04:05", parsed)) exit(1);
    if (macFromString("aa:01:02:03:04:05junk", parsed)) exit(1);
    if (macFromString("100:01:02:03:04:05", parsed)) exit(1);
    if (macFromString("aa:01", parsed)) exit(1);
}

// Regression guard for the process() bounds fixes: truncated LMS packets
// must be ignored, never read past the buffer. Run under ASan to verify.
static void testShortPackets() {
    int stops = 0, conts = 0, codcs = 0, switches = 0;
    SlimProtoClient::Events events;
    events.onStop = [&] { ++stops; };
    events.onCont = [&](uint32_t) { ++conts; };
    events.onCodc = [&](StreamFormat, const PcmParams&) { ++codcs; };
    events.onServerSwitch = [&](uint32_t) { ++switches; };
    SlimProtoClient client({}, "", std::move(events));

    auto withFiller = [](std::string base, size_t total) {
        base.resize(total, '\0');
        return base;
    };

    client.process("");                                  // < 4
    client.process("strm");                              // opcode only
    client.process("strmt");                             // t, 5 < 22 -> ignore
    client.process(withFiller("strmt", 21));             // t, one byte short
    client.process(withFiller("strmp", 21));             // p, one byte short
    client.process(withFiller("strma", 21));             // a, one byte short
    client.process(withFiller("strmu", 21));             // u, one byte short
    client.process(withFiller("audg", 21));              // audg, old code read OOB
    client.process(withFiller("cont", 7));               // cont, one byte short
    client.process(withFiller("codc", 9));               // codc, one byte short
    client.process(withFiller("serv", 7));               // serv, one byte short
    client.process("strmq");                             // q -> onStop
    client.process(withFiller("strmt", 22));             // t at exact bound
    client.process(withFiller("cont", 8));               // cont -> onCont
    client.process(withFiller("codc", 10));              // codc -> onCodc
    client.process(withFiller("serv", 8));               // serv -> onServerSwitch
    if (stops != 1 || conts != 1 || codcs != 1 || switches != 1) {
        fprintf(stderr, "short packet test: stops=%d conts=%d codcs=%d switches=%d\n",
                stops, conts, codcs, switches);
        exit(1);
    }
}

int main() {
    testPackN();
    testPcmCodes();
    testMac();
    testShortPackets();
    printf("ok\n");
    return 0;
}
