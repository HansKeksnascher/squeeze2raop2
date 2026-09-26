// Pins the libxaac-backed AAC decoder: the ADTS path (radio/.aac) and the MP4
// demux path (.m4a), both over embedded fixtures. Compiled only when the
// vendored libxaac is enabled.

#include "playback/decoder/decoder.h"
#include "playback/decoder/mp4_aac_demux.h"

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
using squeeze2raop2::Mp4AacDemuxer;
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

SQ2_TEST(aac_decoder, factory_and_fallback) {
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

SQ2_TEST(aac_decoder, adts_decodes_tone) {
    auto dec = Decoder::create(StreamFormat::Aac, PcmFormat{44100, 16, 2, false}, 0, '2');
    require(dec != nullptr, "aac factory");
    dec->feed(std::span{kFixtureAac});
    dec->finish();
    const auto pcm = drainAll(*dec);
    expectDecoded(*dec, pcm, "adts decoded");
    expect(dec->name() == "aac", "aac name");
}

SQ2_TEST(aac_decoder, adts_chunked_feed) {
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

SQ2_TEST(aac_decoder, adts_chunked_long_stream) {
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

SQ2_TEST(aac_decoder, mp4_demux_decodes_tone) {
    auto dec = Decoder::create(StreamFormat::Aac, PcmFormat{44100, 16, 2, false}, 0, '5');
    require(dec != nullptr, "aac mp4 factory");
    dec->feed(std::span{kFixtureM4a});
    dec->finish();
    const auto pcm = drainAll(*dec);
    expectDecoded(*dec, pcm, "mp4 decoded");
}

// The demuxer is a streaming parser: the same bytes fed in tiny increments must
// produce byte-identical ADTS output to a single whole-file feed. This exercises
// partial box headers, skipRemaining_ spanning feeds, partial mdat samples and
// the compact() boundary that the single-shot decode test never touches.
SQ2_TEST(aac_decoder, mp4_demux_incremental_matches_whole) {
    Mp4AacDemuxer whole;
    whole.feed(std::span{kFixtureM4a});
    whole.finish();
    std::vector<std::byte> wholeOut;
    whole.drain(wholeOut);
    expect(!whole.failed(), "whole-file demux ok");
    require(!wholeOut.empty(), "whole-file demux produced ADTS");

    for (size_t chunk : {size_t{1}, size_t{7}, size_t{64}}) {
        Mp4AacDemuxer inc;
        std::vector<std::byte> incOut;
        for (size_t off = 0; off < kFixtureM4a.size(); off += chunk) {
            const size_t n = std::min(chunk, kFixtureM4a.size() - off);
            inc.feed(std::span{kFixtureM4a}.subspan(off, n));
            inc.drain(incOut);
        }
        inc.finish();
        inc.drain(incOut);
        expect(!inc.failed(), "incremental demux ok");
        expect(incOut == wholeOut, "incremental ADTS matches the whole-file feed");
    }
}

// moov must precede mdat (faststart). An mdat-first container must fail rather
// than emit garbage.
SQ2_TEST(aac_decoder, mp4_demux_mdat_before_moov_fails) {
    std::vector<std::byte> mdat(16, std::byte{0});
    mdat[0] = std::byte{0};
    mdat[1] = std::byte{0};
    mdat[2] = std::byte{0};
    mdat[3] = std::byte{16};  // box size = 16
    mdat[4] = std::byte{'m'};
    mdat[5] = std::byte{'d'};
    mdat[6] = std::byte{'a'};
    mdat[7] = std::byte{'t'};

    Mp4AacDemuxer demux;
    demux.feed(std::span{mdat});
    expect(demux.failed(), "mdat before moov is rejected");
}

#endif  // SQUEEZE2RAOP2_WITH_AAC
