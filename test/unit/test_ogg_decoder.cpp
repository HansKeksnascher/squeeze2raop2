// Pins the stb_vorbis-backed Ogg Vorbis decoder over an embedded fixture.
// Compiled only when the vendored stb_vorbis is enabled.

#include "playback/decoder/decoder.h"

#include "check.h"
#include "decoder_util.h"
#include "lms/slimproto_protocol.h"
#include "ogg_fixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#if defined(SQUEEZE2RAOP2_WITH_OGG)

using namespace squeeze2raop2::test;
using squeeze2raop2::Decoder;
using squeeze2raop2::PcmFormat;
using squeeze2raop2::StreamFormat;

namespace {

void expectDecoded(Decoder& dec, const std::vector<int16_t>& pcm, const char* tag) {
    expect(!dec.hasError(), tag);
    expect(dec.valid(), tag);
    expect(dec.format().sampleRate == 44100, tag);
    expect(dec.format().channels == 2, tag);
    expect(pcm.size() >= 10000, tag);  // the 0.12 s fixture is ~5292 stereo frames
    expect(peak(pcm) > 0, tag);
}

}  // namespace

SQ2_TEST(ogg, factory_and_fallback) {
    const PcmFormat in{44100, 16, 2, false};
    auto dec = Decoder::create(StreamFormat::Ogg, in, 0);
    require(dec != nullptr, "ogg factory");
    // Before the first frame there is no decoded rate: fall back to the input.
    expect(dec->format().sampleRate == 44100, "ogg pre-frame fallback");
    expect(!dec->valid(), "ogg not valid before a frame");
    expect(!dec->hasError(), "ogg not failed before a frame");
    expect(dec->name() == "ogg", "ogg name");
}

SQ2_TEST(ogg, decodes_tone) {
    auto dec = Decoder::create(StreamFormat::Ogg, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "ogg factory");
    dec->feed(std::span{kFixtureOgg});
    dec->finish();
    const auto pcm = drainAll(*dec);
    expectDecoded(*dec, pcm, "ogg decoded");
}

SQ2_TEST(ogg, chunked_feed) {
    auto dec = Decoder::create(StreamFormat::Ogg, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "ogg factory");
    const std::span<const std::byte> all{kFixtureOgg};
    constexpr size_t kChunk = 137;  // deliberately not page-aligned
    std::vector<int16_t> pcm;
    for (size_t off = 0; off < all.size(); off += kChunk) {
        dec->feed(all.subspan(off, std::min(kChunk, all.size() - off)));
        const auto part = drainAll(*dec);
        pcm.insert(pcm.end(), part.begin(), part.end());
    }
    dec->finish();
    const auto tail = drainAll(*dec);
    pcm.insert(pcm.end(), tail.begin(), tail.end());
    expectDecoded(*dec, pcm, "ogg chunked decoded");
}

SQ2_TEST(ogg, long_concatenated_stream) {
    // Mirror the HTTP reader feeding a long stream made of repeated fixtures.
    std::vector<std::byte> big;
    for (int i = 0; i < 50; ++i) big.insert(big.end(), kFixtureOgg.begin(), kFixtureOgg.end());
    auto dec = Decoder::create(StreamFormat::Ogg, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "ogg factory");
    std::vector<int16_t> pcm;
    constexpr size_t kChunk = 4096;
    for (size_t off = 0; off < big.size(); off += kChunk) {
        const size_t n = std::min(kChunk, big.size() - off);
        dec->feed(std::span{big}.subspan(off, n));
        const auto part = drainAll(*dec);
        pcm.insert(pcm.end(), part.begin(), part.end());
    }
    dec->finish();
    const auto tail = drainAll(*dec);
    pcm.insert(pcm.end(), tail.begin(), tail.end());
    expect(!dec->hasError(), "ogg long stream no error");
    expect(pcm.size() >= 4410, "ogg long stream decoded");
}

SQ2_TEST(ogg, mono_is_accepted) {
    // Mono is kept as-is here; AirplayOutput::push() expands it to stereo.
    auto dec = Decoder::create(StreamFormat::Ogg, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "ogg factory");
    dec->feed(std::span{kFixtureOggMono});
    dec->finish();
    const auto pcm = drainAll(*dec);
    expect(!dec->hasError(), "ogg mono no error");
    expect(dec->valid(), "ogg mono valid");
    expect(dec->format().sampleRate == 44100, "ogg mono rate");
    expect(dec->format().channels == 1, "ogg mono channels");
    expect(pcm.size() >= 5000, "ogg mono decoded");
    expect(peak(pcm) > 0, "ogg mono peak");
}

SQ2_TEST(ogg, too_many_channels_rejected) {
    // > 2 channels is an error, matching squeezelite's OGG_ERROR_TOO_MANY_CHANNELS.
    auto dec = Decoder::create(StreamFormat::Ogg, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "ogg factory");
    dec->feed(std::span{kFixtureOgg6ch});
    dec->finish();
    drainAll(*dec);
    expect(dec->hasError(), "ogg 6ch rejected");
}

SQ2_TEST(ogg, truncated_header_fails) {
    auto dec = Decoder::create(StreamFormat::Ogg, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "ogg factory");
    // A prefix of the real stream: not enough to complete the Vorbis headers.
    dec->feed(std::span{kFixtureOgg}.first(10));
    dec->finish();
    expect(dec->hasError(), "ogg truncated header rejected");
}

SQ2_TEST(ogg, garbage_fails) {
    auto dec = Decoder::create(StreamFormat::Ogg, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "ogg factory");
    std::array<std::byte, 1024> junk{};
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<std::byte>((i * 7) & 0xFF);
    dec->feed(junk);
    dec->finish();
    expect(dec->hasError(), "ogg garbage rejected");
}

#endif  // SQUEEZE2RAOP2_WITH_OGG