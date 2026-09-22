// Pins DecodeStage: container header adoption (WAV/AIFF), raw PCM, mono
// normalization and the PCM source-rate regulator. The headers are built by
// hand here (an independent oracle), matching the decoder's own tests.

#include "decode_stage.h"

#include "check.h"
#include "slimproto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace sq2t;
using squeeze2raop2::DecodeStage;
using squeeze2raop2::PcmFormat;
using squeeze2raop2::StreamFormat;

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

std::vector<std::byte> wavWithSamples(const std::vector<int16_t>& samples) {
    std::vector<std::byte> buf(kProbe, std::byte{0});
    putTag(buf.data(), "RIFF");
    putLe32(buf.data() + 4, 0xFFFFFFFFu);
    putTag(buf.data() + 8, "WAVE");
    putTag(buf.data() + 12, "fmt ");
    putLe32(buf.data() + 16, 16);
    putLe16(buf.data() + 20, 1);
    putLe16(buf.data() + 22, 2);
    putLe32(buf.data() + 24, 44100);
    putLe32(buf.data() + 28, 44100 * 2 * 2);
    putLe16(buf.data() + 32, 4);
    putLe16(buf.data() + 34, 16);
    putTag(buf.data() + 36, "data");
    putLe32(buf.data() + 40, static_cast<uint32_t>(samples.size() * 2));
    for (size_t i = 0; i < samples.size(); ++i)
        putLe16(buf.data() + 44 + 2 * i, static_cast<uint16_t>(samples[i]));
    return buf;
}

std::vector<std::byte> aiffHeader() {
    std::vector<std::byte> buf(kProbe, std::byte{0});
    putTag(buf.data(), "FORM");
    putBe32(buf.data() + 4, 0);
    putTag(buf.data() + 8, "AIFF");
    putTag(buf.data() + 12, "COMM");
    putBe32(buf.data() + 16, 18);
    putBe16(buf.data() + 20, 2);
    putBe32(buf.data() + 22, 100);
    putBe16(buf.data() + 26, 16);
    // IEEE 754 80-bit extended for 44100: exponent 0x400E, mantissa 0xAC440000.
    buf[28] = std::byte{0x40};
    buf[29] = std::byte{0x0E};
    buf[30] = std::byte{0xAC};
    buf[31] = std::byte{0x44};
    putTag(buf.data() + 38, "SSND");
    return buf;
}

std::vector<int16_t> drainAll(DecodeStage& stage) {
    std::vector<int16_t> out;
    for (;;) {
        const std::span<const int16_t> chunk = stage.nextChunk();
        if (chunk.empty()) break;
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
}

void testWavStereo() {
    const std::vector<int16_t> samples{100, -200, 3000, -4000, 12345, -12345};
    auto wav = wavWithSamples(samples);

    DecodeStage stage(StreamFormat::Pcm, PcmFormat{}, 0);
    stage.feed(std::as_bytes(std::span{wav}));
    const auto out = drainAll(stage);

    expect(!stage.hasError(), "wav decode reports no error");
    const PcmFormat fmt = stage.format();
    expect(fmt.sampleRate == 44100 && fmt.channels == 2 && fmt.bitsPerSample == 16, "wav fmt");
    expect(out.size() >= samples.size(), "wav produced samples");
    for (size_t i = 0; i < samples.size(); ++i) expect(out[i] == samples[i], "wav sample");
}

void testRawPcmStereo() {
    // The decoder buffers until its 512-byte probe before deciding there is
    // no container, so pad the raw stream; only the leading samples matter.
    const std::vector<int16_t> samples{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<std::byte> raw(kProbe, std::byte{0});
    for (size_t i = 0; i < samples.size(); ++i)
        putLe16(raw.data() + 2 * i, static_cast<uint16_t>(samples[i]));

    DecodeStage stage(StreamFormat::Pcm, PcmFormat{44100, 16, 2, false}, 0);
    stage.feed(std::as_bytes(std::span{raw}));
    const auto out = drainAll(stage);

    expect(out.size() >= samples.size(), "raw pcm produced samples");
    for (size_t i = 0; i < samples.size(); ++i) expect(out[i] == samples[i], "raw pcm sample");
}

void testRawPcmMonoNormalized() {
    const std::vector<int16_t> mono{11, 22, 33};
    std::vector<std::byte> raw(kProbe, std::byte{0});
    for (size_t i = 0; i < mono.size(); ++i)
        putLe16(raw.data() + 2 * i, static_cast<uint16_t>(mono[i]));

    DecodeStage stage(StreamFormat::Pcm, PcmFormat{44100, 16, 1, false}, 0);
    stage.feed(std::as_bytes(std::span{raw}));
    const auto out = drainAll(stage);

    expect(stage.format().channels == 2, "mono normalized to stereo");
    expect(out.size() >= mono.size() * 2, "mono -> stereo sample count");
    for (size_t i = 0; i < mono.size(); ++i)
        expect(out[2 * i] == mono[i] && out[2 * i + 1] == mono[i], "mono duplicated");
}

void testAiffFormat() {
    auto aiff = aiffHeader();

    DecodeStage stage(StreamFormat::Pcm, PcmFormat{}, 0);
    stage.feed(std::as_bytes(std::span{aiff}));
    (void)drainAll(stage);

    expect(!stage.hasError(), "aiff decode reports no error");
    const PcmFormat fmt = stage.format();
    expect(fmt.sampleRate == 44100 && fmt.channels == 2 && fmt.bitsPerSample == 16, "aiff fmt");
}

void testRateRegulation() {
    // 16-bit stereo => 4 bytes/frame. Windows are 10 s; the regulator skips
    // the first (baseline) window, engages beyond 0.1% and releases <=0.045%.
    DecodeStage stage(StreamFormat::Pcm, PcmFormat{44100, 16, 2, false}, 44100);

    expect(stage.observeOutput(200000, 1760000, 1000) == 0.0, "regulator: mark");
    expect(stage.observeOutput(200000, 3520000, 11000) == 0.0, "regulator: baseline window");
    // fps = 1760000 / 4 / 10 = 44000 (100 off) -> engage.
    const double engaged = stage.observeOutput(200000, 5280000, 21000);
    expect(engaged > 43999.0 && engaged < 44001.0, "regulator engaged");
    // same rate again: holds.
    expect(stage.observeOutput(200000, 7040000, 31000) > 43999.0, "regulator holds");
    // fps = 44100 -> within 0.045% -> release to pass-through.
    expect(stage.observeOutput(200000, 8804000, 41000) == 0.0, "regulator released");
}

}  // namespace

int main() {
    testWavStereo();
    testRawPcmStereo();
    testRawPcmMonoNormalized();
    testAiffFormat();
    testRateRegulation();
    std::printf("ok\n");
    return 0;
}
