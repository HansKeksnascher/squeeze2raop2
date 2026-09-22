// Pins the Decoder base: the shared chunk buffer behind nextChunk(), the
// input-format fallback for a not-yet-valid decoder, and the PCM source-rate
// regulator. Container-header adoption and normalization are pinned
// separately in test_pcm_decoder.cpp.

#include "decoder/decoder.h"

#include "check.h"
#include "slimproto.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace sq2t;
using squeeze2raop2::Decoder;
using squeeze2raop2::PcmFormat;
using squeeze2raop2::StreamFormat;

namespace {

constexpr size_t kProbe = 512;

void putLe16(std::byte* p, uint16_t v) {
    p[0] = static_cast<std::byte>(v & 0xFF);
    p[1] = static_cast<std::byte>((v >> 8) & 0xFF);
}

std::vector<int16_t> drainAll(Decoder& dec) {
    std::vector<int16_t> out;
    for (;;) {
        const std::span<const int16_t> chunk = dec.nextChunk();
        if (chunk.empty()) break;
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
}

void testRawPcmStereoChunks() {
    // The decoder buffers until its 512-byte probe before deciding there is no
    // container, so pad the raw stream; only the leading samples matter.
    const std::vector<int16_t> samples{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<std::byte> raw(kProbe, std::byte{0});
    for (size_t i = 0; i < samples.size(); ++i)
        putLe16(raw.data() + 2 * i, static_cast<uint16_t>(samples[i]));

    auto dec = Decoder::create(StreamFormat::Pcm, PcmFormat{44100, 16, 2, false}, 0);
    expect(dec != nullptr, "pcm factory");
    dec->feed(std::as_bytes(std::span{raw}));
    const auto out = drainAll(*dec);

    expect(out.size() >= samples.size(), "raw pcm produced samples");
    for (size_t i = 0; i < samples.size(); ++i) expect(out[i] == samples[i], "raw pcm sample");
    const PcmFormat fmt = dec->format();
    expect(fmt.sampleRate == 44100 && fmt.channels == 2, "raw pcm fmt");
}

void testRawPcmMonoNormalized() {
    const std::vector<int16_t> mono{11, 22, 33};
    std::vector<std::byte> raw(kProbe, std::byte{0});
    for (size_t i = 0; i < mono.size(); ++i)
        putLe16(raw.data() + 2 * i, static_cast<uint16_t>(mono[i]));

    auto dec = Decoder::create(StreamFormat::Pcm, PcmFormat{44100, 16, 1, false}, 0);
    dec->feed(std::as_bytes(std::span{raw}));
    const auto out = drainAll(*dec);

    expect(dec->format().channels == 2, "mono normalized to stereo");
    expect(out.size() >= mono.size() * 2, "mono -> stereo sample count");
    for (size_t i = 0; i < mono.size(); ++i)
        expect(out[2 * i] == mono[i] && out[2 * i + 1] == mono[i], "mono duplicated");
}

void testMp3FormatFallback() {
    // Before its first frame an MP3 has no rate; format() falls back to the
    // strm-derived input format.
    auto dec = Decoder::create(StreamFormat::Mp3, PcmFormat{44100, 16, 2, false}, 0);
    expect(dec != nullptr, "mp3 factory");
    const PcmFormat fmt = dec->format();
    expect(fmt.sampleRate == 44100 && fmt.channels == 2, "mp3 pre-frame fallback");
}

void testRateRegulation() {
    // 16-bit stereo => 4 bytes/frame. The regulator skips the first
    // (baseline) window, engages beyond 0.1% and releases below 0.045%.
    auto dec = Decoder::create(StreamFormat::Pcm, PcmFormat{44100, 16, 2, false}, 44100);
    expect(dec != nullptr, "regulated pcm factory");
    expect(!dec->regulating(), "regulator starts passive");

    // baseline: prevReceived == 0, so this window only primes the counter.
    expect(dec->regulateRate(1760000, 200000, 10000) == 0.0, "regulator: baseline window");
    // fps = 1760000 / 4 / 10 = 44000 (100 off) -> engage.
    const double engaged = dec->regulateRate(3520000, 200000, 10000);
    expect(engaged > 43999.0 && engaged < 44001.0, "regulator engaged");
    expect(dec->regulating(), "regulator engaged state");
    // Same rate again: holds.
    expect(dec->regulateRate(5280000, 200000, 10000) > 43999.0, "regulator holds");
    // fps = 1764000 / 4 / 10 = 44100 -> within 0.045% -> release.
    expect(dec->regulateRate(7044000, 200000, 10000) == 0.0, "regulator released");
    expect(!dec->regulating(), "regulator released state");
}

}  // namespace

int main() {
    testRawPcmStereoChunks();
    testRawPcmMonoNormalized();
    testMp3FormatFallback();
    testRateRegulation();
    std::printf("ok\n");
    return 0;
}
