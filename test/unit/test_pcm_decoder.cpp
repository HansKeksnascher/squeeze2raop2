// Pins PcmDecoder's container-header parsing (WAV little-endian, AIFF
// big-endian) and the s16 LE stereo normalization. The headers are built by
// hand here (not via any production helper) so this stays an independent
// oracle over the endian reads that moved into byte_order.h.

#include "playback/decoder/pcm_decoder.h"

#include "check.h"
#include "lms/slimproto_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace squeeze2raop2::test;
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
    putBe32(buf.data() + 16, 18);   // COMM chunk size
    putBe16(buf.data() + 20, 2);    // channels
    putBe32(buf.data() + 22, 100);  // sample frames
    putBe16(buf.data() + 26, 16);   // bits per sample
    // IEEE 754 80-bit extended for 44100: exponent 0x400E, mantissa 0xAC440000.
    buf[28] = std::byte{0x40};
    buf[29] = std::byte{0x0E};
    buf[30] = std::byte{0xAC};
    buf[31] = std::byte{0x44};
    putTag(buf.data() + 38, "SSND");
    return buf;
}

}  // namespace

SQ2_TEST(pcm_decoder, wav_decode) {
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

// Raw (no container) endian/bit-depth conversion branches that no other test
// feeds samples through: 16-bit big-endian and 24-bit (LE and BE) stereo, and
// 16-bit big-endian mono expansion. The probe needs 512 buffered bytes before
// the header decision, so the buffer is zero-padded.
SQ2_TEST(pcm_decoder, raw_s16_be_stereo) {
    std::vector<std::byte> raw(kProbe, std::byte{0});
    raw[0] = static_cast<std::byte>(0x12);
    raw[1] = static_cast<std::byte>(0x34);
    raw[2] = static_cast<std::byte>(0xAB);
    raw[3] = static_cast<std::byte>(0xCD);

    PcmDecoder dec(PcmFormat{44100, 16, 2, true}, 0);
    dec.feed(std::as_bytes(std::span{raw}));
    std::vector<int16_t> out(kProbe, 0);
    const size_t n = dec.drain(out);

    expect(!dec.hasError(), "be s16 no error");
    expect(n >= 2, "be s16 produced samples");
    expect(out[0] == 0x1234, "be s16 first sample swapped");
    expect(out[1] == static_cast<int16_t>(0xABCD), "be s16 second sample swapped");
}

SQ2_TEST(pcm_decoder, raw_s16_be_mono) {
    std::vector<std::byte> raw(kProbe, std::byte{0});
    raw[0] = static_cast<std::byte>(0x12);
    raw[1] = static_cast<std::byte>(0x34);

    PcmDecoder dec(PcmFormat{44100, 16, 1, true}, 0);
    dec.feed(std::as_bytes(std::span{raw}));
    std::vector<int16_t> out(kProbe, 0);
    dec.drain(out);

    expect(out[0] == 0x1234 && out[1] == 0x1234, "be mono expanded to stereo");
}

SQ2_TEST(pcm_decoder, raw_s24_le_stereo) {
    std::vector<std::byte> raw(kProbe, std::byte{0});
    // 0x123456 and 0xABCDEF, little-endian 24-bit.
    raw[0] = static_cast<std::byte>(0x56);
    raw[1] = static_cast<std::byte>(0x34);
    raw[2] = static_cast<std::byte>(0x12);
    raw[3] = static_cast<std::byte>(0xEF);
    raw[4] = static_cast<std::byte>(0xCD);
    raw[5] = static_cast<std::byte>(0xAB);

    PcmDecoder dec(PcmFormat{44100, 24, 2, false}, 0);
    dec.feed(std::as_bytes(std::span{raw}));
    std::vector<int16_t> out(kProbe, 0);
    dec.drain(out);

    expect(!dec.hasError(), "le s24 no error");
    expect(out[0] == 0x1234, "le s24 takes the top 16 bits");
    expect(out[1] == static_cast<int16_t>(0xABCD), "le s24 second sample top bits");
}

SQ2_TEST(pcm_decoder, raw_s24_be_stereo) {
    std::vector<std::byte> raw(kProbe, std::byte{0});
    // 0x123456 and 0xABCDEF, big-endian 24-bit.
    raw[0] = static_cast<std::byte>(0x12);
    raw[1] = static_cast<std::byte>(0x34);
    raw[2] = static_cast<std::byte>(0x56);
    raw[3] = static_cast<std::byte>(0xAB);
    raw[4] = static_cast<std::byte>(0xCD);
    raw[5] = static_cast<std::byte>(0xEF);

    PcmDecoder dec(PcmFormat{44100, 24, 2, true}, 0);
    dec.feed(std::as_bytes(std::span{raw}));
    std::vector<int16_t> out(kProbe, 0);
    dec.drain(out);

    expect(!dec.hasError(), "be s24 no error");
    expect(out[0] == 0x1234, "be s24 takes the top 16 bits");
    expect(out[1] == static_cast<int16_t>(0xABCD), "be s24 second sample top bits");
}

// A raw s16 LE stereo ramp, `frames` frames. channel0 = k-2000, channel1 =
// -(k-2000).
std::vector<std::byte> rawRamp(size_t frames) {
    std::vector<std::byte> raw(frames * 4);
    for (size_t k = 0; k < frames; ++k) {
        const int16_t v = static_cast<int16_t>(static_cast<int>(k) - 2000);
        const int16_t w = static_cast<int16_t>(-v);
        const uint16_t uv = static_cast<uint16_t>(v);
        const uint16_t uw = static_cast<uint16_t>(w);
        raw[4 * k + 0] = static_cast<std::byte>(uv & 0xFF);
        raw[4 * k + 1] = static_cast<std::byte>((uv >> 8) & 0xFF);
        raw[4 * k + 2] = static_cast<std::byte>(uw & 0xFF);
        raw[4 * k + 3] = static_cast<std::byte>((uw >> 8) & 0xFF);
    }
    return raw;
}

// Disengaged (step 1.0) the rate stage is a bit-exact move: every input frame
// comes out unchanged in order.
SQ2_TEST(pcm_decoder, rate_stage_bypass_bit_exact) {
    constexpr size_t kFrames = 4000;
    const auto raw = rawRamp(kFrames);
    PcmDecoder dec(PcmFormat{44100, 16, 2, false}, 44100);
    dec.feed(std::as_bytes(std::span{raw}));

    std::vector<int16_t> out(kFrames * 2, 0);
    const size_t n = dec.drain(out);
    expect(!dec.hasError(), "bypass no error");
    expect(n == kFrames * 2, "bypass emits every frame");
    for (size_t k = 0; k < kFrames; ++k)
        expect(out[2 * k] == static_cast<int16_t>(k - 2000) &&
                   out[2 * k + 1] == -static_cast<int16_t>(k - 2000),
               "bypass passthrough");
}

// Engaged, the stage consumes rateStep_ source frames per output frame via
// linear interpolation. Pin the output against an independent reference over
// the whole run, which catches a broken phase/compaction across the internal
// kChunkFrames boundaries.
SQ2_TEST(pcm_decoder, rate_stage_interpolates) {
    constexpr size_t kFrames = 4000;
    const auto raw = rawRamp(kFrames);
    PcmDecoder dec(PcmFormat{44100, 16, 2, false}, 44100);
    dec.feed(std::as_bytes(std::span{raw}));
    dec.setSourceRate(44100.0 * 1.01);
    // Mirror PcmDecoder::setSourceRate's clamp so the reference phase matches
    // the implementation bit-for-bit (1.01 is not exact in binary).
    const double kStep = std::clamp(44100.0 * 1.01 / 44100.0, 0.99, 1.01);

    std::vector<int16_t> out(kFrames * 2 + 4096, 0);
    const size_t n = dec.drain(out);
    expect(!dec.hasError(), "regulated no error");
    require(n > 0, "regulated produced samples");

    // Independent reference over the same interpolation. Phase accumulation is
    // compared with a small tolerance: the implementation accumulates per
    // kChunkFrames chunk (adding then subtracting the consumed integer part),
    // which can differ from one running sum by an ulp and flip a truncation.
    auto src0 = [](size_t k) { return static_cast<int16_t>(static_cast<int>(k) - 2000); };
    size_t produced = 0;
    double phase = 0.0;
    while (static_cast<size_t>(phase) + 1 < kFrames && produced * 2 + 1 < n) {
        const size_t i0 = static_cast<size_t>(phase);
        const double frac = phase - static_cast<double>(i0);
        const int16_t a0 = src0(i0), b0 = src0(i0 + 1);
        const int16_t a1 = static_cast<int16_t>(-a0), b1 = static_cast<int16_t>(-b0);
        const double e0 = a0 + (b0 - a0) * frac;
        const double e1 = a1 + (b1 - a1) * frac;
        expect(std::abs(out[produced * 2] - e0) <= 2.0, "regulated lerp ch0");
        expect(std::abs(out[produced * 2 + 1] - e1) <= 2.0, "regulated lerp ch1");
        ++produced;
        phase += kStep;
    }
    // The output should shrink by ~1/step; allow a couple of frames of slack.
    const size_t expected = static_cast<size_t>(static_cast<double>(kFrames - 1) / kStep) + 1;
    expect(n / 2 + 2 >= expected && n / 2 <= expected + 2,
           "regulated emits the interpolated count");
}

SQ2_TEST(pcm_decoder, aiff_header) {
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

// A COMM chunk whose body (the rate u32 at +18) runs past the probe buffer must
// be rejected without reading past the end: run under ASan, the old +18 guard
// read four bytes out of bounds here. A JUNK chunk jumps the walk to a COMM
// whose +22 reaches past the buffer end.
SQ2_TEST(pcm_decoder, aiff_truncated_comm_fails_closed) {
    std::vector<std::byte> buf(kProbe, std::byte{0});
    putTag(buf.data(), "FORM");
    putTag(buf.data() + 8, "AIFF");
    const size_t off = kProbe - 20;  // off + 18 <= size, off + 22 > size
    putTag(buf.data() + 12, "JUNK");
    putBe32(buf.data() + 16, static_cast<uint32_t>(off - 20));  // 12 + len + 8 = off
    putTag(buf.data() + off, "COMM");

    PcmDecoder dec(PcmFormat{}, 0);
    dec.feed(std::as_bytes(std::span{buf}));
    std::vector<int16_t> out(64, 0);
    (void)dec.drain(out);

    expect(dec.hasError(), "truncated AIFF COMM rejected without an OOB read");
}
