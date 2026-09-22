// Pins PcmDecoder's container-header parsing (WAV little-endian, AIFF
// big-endian) and the s16 LE stereo normalization. The headers are built by
// hand here (not via any production helper) so this stays an independent
// oracle over the endian reads that moved into byte_order.h.

#include "decoder/pcm_decoder.h"

#include "check.h"
#include "slimproto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace sq2t;
using squeeze2raop2::PcmDecoder;
using squeeze2raop2::PcmFormat;

namespace {

// The decoder defers its container decision until this many bytes are buffered.
constexpr size_t kProbe = 512;

void putLe16(std::byte* p, uint16_t v) {
    p[0] = static_cast<std::byte>(v & 0xFF);
    p[1] = static_cast<std::byte>((v >> 8) & 0xFF);
}
void putLe32(std::byte* p, uint32_t v) {
    putLe16(p, static_cast<uint16_t>(v & 0xFFFF));
    putLe16(p + 2, static_cast<uint16_t>(v >> 16));
}
void putBe16(std::byte* p, uint16_t v) {
    p[0] = static_cast<std::byte>((v >> 8) & 0xFF);
    p[1] = static_cast<std::byte>(v & 0xFF);
}
void putBe32(std::byte* p, uint32_t v) {
    putBe16(p, static_cast<uint16_t>(v >> 16));
    putBe16(p + 2, static_cast<uint16_t>(v & 0xFFFF));
}
void putTag(std::byte* p, const char (&tag)[5]) {
    for (size_t i = 0; i < 4; ++i) p[i] = static_cast<std::byte>(tag[i]);
}

// 44-byte RIFF/WAVE header + interleaved s16 LE samples, zero-padded to the
// probe size.
std::vector<std::byte> wavWithSamples(const std::vector<int16_t>& samples) {
    std::vector<std::byte> buf(kProbe, std::byte{0});
    putTag(buf.data(), "RIFF");
    putLe32(buf.data() + 4, 0xFFFFFFFFu);
    putTag(buf.data() + 8, "WAVE");
    putTag(buf.data() + 12, "fmt ");
    putLe32(buf.data() + 16, 16);  // fmt chunk size
    putLe16(buf.data() + 20, 1);   // PCM
    putLe16(buf.data() + 22, 2);   // channels
    putLe32(buf.data() + 24, 44100);
    putLe32(buf.data() + 28, 44100 * 2 * 2);  // byte rate
    putLe16(buf.data() + 32, 4);              // block align
    putLe16(buf.data() + 34, 16);             // bits per sample
    putTag(buf.data() + 36, "data");
    putLe32(buf.data() + 40, static_cast<uint32_t>(samples.size() * 2));
    for (size_t i = 0; i < samples.size(); ++i)
        putLe16(buf.data() + 44 + 2 * i, static_cast<uint16_t>(samples[i]));
    return buf;
}

// FORM/AIFF: COMM (44100 Hz, 2 ch, 16 bit via the 80-bit extended rate) then
// SSND. Only the format adoption is asserted (the skip arithmetic mirrors the
// pcm.c reference and is not endian-sensitive).
std::vector<std::byte> aiffHeader() {
    std::vector<std::byte> buf(kProbe, std::byte{0});
    putTag(buf.data(), "FORM");
    putBe32(buf.data() + 4, 0);
    putTag(buf.data() + 8, "AIFF");
    putTag(buf.data() + 12, "COMM");
    putBe32(buf.data() + 16, 18);  // COMM chunk size
    putBe16(buf.data() + 20, 2);   // channels
    putBe32(buf.data() + 22, 100); // sample frames
    putBe16(buf.data() + 26, 16);  // bits per sample
    // IEEE 754 80-bit extended for 44100: exponent 0x400E, mantissa 0xAC440000.
    buf[28] = std::byte{0x40};
    buf[29] = std::byte{0x0E};
    buf[30] = std::byte{0xAC};
    buf[31] = std::byte{0x44};
    putTag(buf.data() + 38, "SSND");
    return buf;
}

void testWavDecode() {
    const std::vector<int16_t> samples{100, -200, 3000, -4000, 12345, -12345};
    auto wav = wavWithSamples(samples);

    PcmDecoder dec(PcmFormat{}, 0);  // 0 = adopt the source rate
    dec.feed(std::as_bytes(std::span{wav}));
    std::vector<int16_t> out(samples.size() * 2 + 8, 0);
    const size_t n = dec.drain(out);

    expect(!dec.hasError(), "wav decode reports no error");
    expect(dec.valid(), "wav header adopted");
    const PcmFormat fmt = dec.format();
    expect(fmt.sampleRate == 44100 && fmt.channels == 2 && fmt.bitsPerSample == 16, "wav fmt");
    expect(n >= samples.size(), "wav drain produced the request");
    for (size_t i = 0; i < samples.size(); ++i)
        expect(out[i] == samples[i], "wav s16 LE sample passthrough");
}

void testAiffHeader() {
    auto aiff = aiffHeader();

    PcmDecoder dec(PcmFormat{}, 0);
    dec.feed(std::as_bytes(std::span{aiff}));
    std::vector<int16_t> out(64, 0);
    (void)dec.drain(out);

    expect(!dec.hasError(), "aiff decode reports no error");
    expect(dec.valid(), "aiff header adopted");
    const PcmFormat fmt = dec.format();
    expect(fmt.sampleRate == 44100 && fmt.channels == 2 && fmt.bitsPerSample == 16, "aiff fmt");
}

}  // namespace

int main() {
    testWavDecode();
    testAiffHeader();
    std::printf("ok\n");
    return 0;
}