// Pins the libogg + libopus-backed Ogg Opus decoder over embedded fixtures.
// Compiled only when the vendored libopus is enabled.

#include "playback/decoder/decoder.h"

#include "check.h"
#include "decoder_util.h"
#include "lms/wire_types.h"
#include "opus_fixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#if defined(SQUEEZE2RAOP2_WITH_OPUS)

using namespace squeeze2raop2::test;
using squeeze2raop2::Decoder;
using squeeze2raop2::PcmFormat;
using squeeze2raop2::StreamFormat;

namespace {

void expectDecoded(Decoder& dec, const std::vector<int16_t>& pcm, const char* tag) {
    expect(!dec.hasError(), tag);
    expect(dec.valid(), tag);
    expect(dec.format().sampleRate == 48000, tag);
    // 0.12 s at 48 kHz is 5760 frames per channel; the ffmpeg reference
    // decodes the same fixture to exactly 5760 frames, so this pins pre-skip
    // and end-trim correctness.
    expect(pcm.size() == 11520, tag);
    expect(peak(pcm) > 0, tag);
}

}  // namespace

SQ2_TEST(opus, factory_and_fallback) {
    const PcmFormat in{44100, 16, 2, false};
    auto dec = Decoder::create(StreamFormat::Opus, in, 0);
    require(dec != nullptr, "opus factory");
    // Before the OpusHead there is no decoded rate: fall back to the input.
    expect(dec->format().sampleRate == 44100, "opus pre-header fallback");
    expect(!dec->valid(), "opus not valid before the header");
    expect(!dec->hasError(), "opus not failed before the header");
    expect(dec->name() == "opus", "opus name");
}

SQ2_TEST(opus, decodes_tone) {
    auto dec = Decoder::create(StreamFormat::Opus, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "opus factory");
    dec->feed(std::span{kFixtureOpus});
    dec->finish();
    const auto pcm = drainAll(*dec);
    fprintf(stderr, "OPUSPCM samples=%zu frames=%zu\n", pcm.size(), pcm.size() / 2);
    expectDecoded(*dec, pcm, "opus decoded");
    expect(dec->format().channels == 2, "opus stereo");
}

SQ2_TEST(opus, chunked_feed) {
    auto dec = Decoder::create(StreamFormat::Opus, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "opus factory");
    const std::span<const std::byte> all{kFixtureOpus};
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
    expectDecoded(*dec, pcm, "opus chunked decoded");
}

SQ2_TEST(opus, byte_at_a_time) {
    // One byte per feed stresses page reassembly across segment boundaries.
    auto dec = Decoder::create(StreamFormat::Opus, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "opus factory");
    std::vector<int16_t> pcm;
    for (const std::byte b : kFixtureOpus) {
        const std::byte one[1] = {b};
        dec->feed(one);
        const auto part = drainAll(*dec);
        pcm.insert(pcm.end(), part.begin(), part.end());
    }
    dec->finish();
    const auto tail = drainAll(*dec);
    pcm.insert(pcm.end(), tail.begin(), tail.end());
    expectDecoded(*dec, pcm, "opus byte-at-a-time decoded");
}

SQ2_TEST(opus, mono_is_accepted) {
    // Mono is kept as-is here; AirplayOutput::push() expands it to stereo.
    auto dec = Decoder::create(StreamFormat::Opus, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "opus factory");
    dec->feed(std::span{kFixtureOpusMono});
    dec->finish();
    const auto pcm = drainAll(*dec);
    expect(!dec->hasError(), "opus mono no error");
    expect(dec->valid(), "opus mono valid");
    expect(dec->format().sampleRate == 48000, "opus mono rate");
    expect(dec->format().channels == 1, "opus mono channels");
    expect(pcm.size() >= 5000, "opus mono decoded");
    expect(peak(pcm) > 0, "opus mono peak");
}

SQ2_TEST(opus, too_many_channels_rejected) {
    // 6 channels is mapping family 1, which this decoder does not support.
    auto dec = Decoder::create(StreamFormat::Opus, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "opus factory");
    dec->feed(std::span{kFixtureOpus6ch});
    dec->finish();
    drainAll(*dec);
    expect(dec->hasError(), "opus 6ch rejected");
}

SQ2_TEST(opus, truncated_header_fails) {
    auto dec = Decoder::create(StreamFormat::Opus, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "opus factory");
    // A prefix of the real stream: not enough to complete the OpusHead.
    dec->feed(std::span{kFixtureOpus}.first(10));
    dec->finish();
    expect(dec->hasError(), "opus truncated header rejected");
}

SQ2_TEST(opus, garbage_fails) {
    auto dec = Decoder::create(StreamFormat::Opus, PcmFormat{44100, 16, 2, false}, 0);
    require(dec != nullptr, "opus factory");
    // Valid Ogg framing around junk is not Opus; a plain junk blob is not even
    // Ogg. Either way the decoder must not claim success.
    std::array<std::byte, 1024> junk{};
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<std::byte>((i * 7) & 0xFF);
    dec->feed(junk);
    dec->finish();
    expect(dec->hasError(), "opus garbage rejected");
}

#endif  // SQUEEZE2RAOP2_WITH_OPUS