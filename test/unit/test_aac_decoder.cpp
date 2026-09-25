// Pins the libxaac-backed AAC decoder: the ADTS path (radio/.aac) and the MP4
// demux path (.m4a), both over embedded fixtures. Compiled only when the
// vendored libxaac is enabled.

#include "playback/decoder/decoder.h"

#include "aac_fixture.h"
#include "check.h"
#include "decoder_util.h"
#include "lms/slimproto_protocol.h"
#include "m4a_fixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#if defined(SQUEEZE2RAOP2_WITH_AAC)

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
    expect(pcm.size() >= 4410, tag);  // ~0.05 s of stereo frames at least
    expect(peak(pcm) > 0, tag);
}

}  // namespace

SQ2_TEST(aac, factory_and_fallback) {
    const PcmFormat in{44100, 16, 2, false};
    auto adts = Decoder::create(StreamFormat::Aac, in, 0, '2');
    require(adts != nullptr, "aac adts factory");
    // Before the first frame there is no decoded rate: fall back to the input.
    expect(adts->format().sampleRate == 44100, "aac pre-frame fallback");
    expect(!adts->valid(), "aac not valid before a frame");

    auto mp4 = Decoder::create(StreamFormat::Aac, in, 0, '5');
    require(mp4 != nullptr, "aac mp4 factory");

    // ADIF / LATM transports are not supported.
    auto adif = Decoder::create(StreamFormat::Aac, in, 0, '1');
    require(adif != nullptr, "aac adif factory constructs");
    expect(adif->hasError(), "aac adif rejected");
}

SQ2_TEST(aac, adts_decodes_tone) {
    auto dec = Decoder::create(StreamFormat::Aac, PcmFormat{44100, 16, 2, false}, 0, '2');
    require(dec != nullptr, "aac factory");
    dec->feed(std::span{kFixtureAac});
    dec->finish();
    const auto pcm = drainAll(*dec);
    expectDecoded(*dec, pcm, "adts decoded");
    expect(dec->name() == "aac", "aac name");
}

SQ2_TEST(aac, adts_chunked_feed) {
    auto dec = Decoder::create(StreamFormat::Aac, PcmFormat{44100, 16, 2, false}, 0, '2');
    require(dec != nullptr, "aac factory");
    const std::span<const std::byte> all{kFixtureAac};
    constexpr size_t kChunk = 137;  // deliberately not frame-aligned
    std::vector<int16_t> pcm;
    for (size_t off = 0; off < all.size(); off += kChunk) {
        dec->feed(all.subspan(off, std::min(kChunk, all.size() - off)));
        const auto part = drainAll(*dec);
        pcm.insert(pcm.end(), part.begin(), part.end());
    }
    dec->finish();
    const auto tail = drainAll(*dec);
    pcm.insert(pcm.end(), tail.begin(), tail.end());
    expectDecoded(*dec, pcm, "adts chunked decoded");
}

SQ2_TEST(aac, adts_long_stream) {
    auto dec = Decoder::create(StreamFormat::Aac, PcmFormat{44100, 16, 2, false}, 0, '2');
    require(dec != nullptr, "aac factory");
    std::vector<int16_t> pcm;
    for (int rep = 0; rep < 50; ++rep) {
        dec->feed(std::span{kFixtureAac});
        const auto part = drainAll(*dec);
        pcm.insert(pcm.end(), part.begin(), part.end());
    }
    dec->finish();
    const auto tail = drainAll(*dec);
    pcm.insert(pcm.end(), tail.begin(), tail.end());
    expect(!dec->hasError(), "long stream no error");
    // The fixture is 7 ADTS frames (~7168 stereo frames); every repetition must
    // decode, so 50 reps is ~716800 samples.
    expect(pcm.size() >= 50 * 14000, "long stream decoded every repetition");
}

SQ2_TEST(aac, adts_chunked_long_stream) {
    // Mirror the HTTP reader's ~4 KB chunks over a long concatenated stream.
    std::vector<std::byte> big;
    for (int i = 0; i < 200; ++i) big.insert(big.end(), kFixtureAac.begin(), kFixtureAac.end());
    auto dec = Decoder::create(StreamFormat::Aac, PcmFormat{44100, 16, 2, false}, 0, '2');
    require(dec != nullptr, "aac factory");
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
    expect(!dec->hasError(), "chunked long no error");
    expect(pcm.size() >= 200 * 14000, "chunked long decoded every repetition");
}

SQ2_TEST(aac, mp4_demux_decodes_tone) {
    auto dec = Decoder::create(StreamFormat::Aac, PcmFormat{44100, 16, 2, false}, 0, '5');
    require(dec != nullptr, "aac mp4 factory");
    dec->feed(std::span{kFixtureM4a});
    dec->finish();
    const auto pcm = drainAll(*dec);
    expectDecoded(*dec, pcm, "mp4 decoded");
}

#endif  // SQUEEZE2RAOP2_WITH_AAC
