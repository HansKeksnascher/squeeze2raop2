#include "slimproto.h"
#include "util.h"

#include <cstdio>
#include <cstring>

using namespace sq2;

static void testPackN() {
    uint8_t buf[8] = {};
    packN(buf, 0x0102030405060708ULL, 8);
    if (buf[0] != 0x01 || buf[1] != 0x02 || buf[6] != 0x07 || buf[7] != 0x08) {
        fprintf(stderr, "packN byte order wrong\n");
        exit(1);
    }
    packN(buf, 0x1234, 2);
    if (buf[0] != 0x12 || buf[1] != 0x34) exit(1);
    if (unpackN(buf, 2) != 0x1234) exit(1);
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
}

int main() {
    testPackN();
    testPcmCodes();
    testMac();
    printf("ok\n");
    return 0;
}
