// Pins the Decoder base: the shared chunk buffer behind nextChunk() and the
// input-format fallback for a not-yet-valid decoder. Container-header adoption
// and normalization are pinned separately in test_pcm_decoder.cpp. The MP3 case
// exercises the real minimp3 path (feed -> finish -> drain) over an embedded
// tone fixture when built in.

#include "playback/decoder/decoder.h"

#include "check.h"
#include "decoder_util.h"
#include "lms/slimproto_protocol.h"
#if defined(SQUEEZE2RAOP2_WITH_MP3)
#include "mp3_fixture.h"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

using namespace squeeze2raop2::test;
using squeeze2raop2::Decoder;
using squeeze2raop2::PcmFormat;
using squeeze2raop2::StreamFormat;

namespace {

constexpr size_t kProbe = 512;

void putLe16(std::byte* p, uint16_t v) {
    p[0] = static_cast<std::byte>(v & 0xFF);
    p[1] = static_cast<std::byte>((v >> 8) & 0xFF);
}

}  // namespace

SQ2_TEST(decoder, raw_pcm_stereo_chunks) {
    // The decoder buffers until its 512-byte probe before deciding there is no
    // container, so pad the raw stream; only the leading samples matter.
    const std::vector<int16_t> samples{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<std::byte> raw(kProbe, std::byte{0});
    for (size_t i = 0; i < samples.size(); ++i)
        putLe16(raw.data() + 2 * i, static_cast<uint16_t>(samples[i]));

    auto dec = Decoder::create(StreamFormat::Pcm, PcmFormat{44100, 16, 2, false});
    require(dec != nullptr, "pcm factory");
    dec->feed(std::as_bytes(std::span{raw}));
    const auto out = chunksAll(*dec);

    expect(out.size() >= samples.size(), "raw pcm produced samples");
    for (size_t i = 0; i < samples.size(); ++i) expect(out[i] == samples[i], "raw pcm sample");
    const PcmFormat fmt = dec->format();
    expect(fmt.sampleRate == 44100 && fmt.channels == 2, "raw pcm fmt");
}

SQ2_TEST(decoder, raw_pcm_mono_normalized) {
    const std::vector<int16_t> mono{11, 22, 33};
    std::vector<std::byte> raw(kProbe, std::byte{0});
    for (size_t i = 0; i < mono.size(); ++i)
        putLe16(raw.data() + 2 * i, static_cast<uint16_t>(mono[i]));

    auto dec = Decoder::create(StreamFormat::Pcm, PcmFormat{44100, 16, 1, false});
    dec->feed(std::as_bytes(std::span{raw}));
    const auto out = chunksAll(*dec);

    expect(dec->format().channels == 2, "mono normalized to stereo");
    expect(out.size() >= mono.size() * 2, "mono -> stereo sample count");
    for (size_t i = 0; i < mono.size(); ++i)
        expect(out[2 * i] == mono[i] && out[2 * i + 1] == mono[i], "mono duplicated");
}

#if defined(SQUEEZE2RAOP2_WITH_MP3)
SQ2_TEST(decoder, mp3_format_fallback) {
    // Before its first frame an MP3 has no rate; format() falls back to the
    // strm-derived input format.
    auto dec = Decoder::create(StreamFormat::Mp3, PcmFormat{44100, 16, 2, false});
    expect(dec != nullptr, "mp3 factory");
    const PcmFormat fmt = dec->format();
    expect(fmt.sampleRate == 44100 && fmt.channels == 2, "mp3 pre-frame fallback");
}

SQ2_TEST(decoder, mp3_decodes_frame_and_flushes_tail) {
    auto dec = Decoder::create(StreamFormat::Mp3, PcmFormat{44100, 16, 2, false});
    require(dec != nullptr, "mp3 factory");

    dec->feed(std::span{kFixtureMp3});
    dec->finish();  // tail-frame flush
    const auto pcm = drainAll(*dec);

    expect(!dec->hasError(), "no decode error");
    expect(dec->valid(), "header accepted");
    expect(dec->format().sampleRate == 44100, "detected rate");
    expect(dec->format().channels == 2, "detected channels");
    // The fixture is ~6 MPEG-1 frames (~6900 samples); requiring well over one
    // frame proves the whole stream decoded and finish() flushed the tail.
    expect(pcm.size() >= 6000, "every MPEG-1 frame decoded, tail flushed");
    expect(peak(pcm) > 0, "non-silent decode");
}
#endif  // SQUEEZE2RAOP2_WITH_MP3

SQ2_TEST(decoder, supported_codec_table_matches_factory) {
    // The HELO caps, the strm guard and the factory all read this one list.
    // Pin the exact cap tokens and their advertised order (the wire contract
    // LMS sees), that every entry actually builds a decoder, and that formats
    // outside the list have no factory.
    using squeeze2raop2::CodecInfo;
    using squeeze2raop2::supportedCodecs;
    using squeeze2raop2::supportsFormat;

    std::string expected = "pcm";
#if defined(SQUEEZE2RAOP2_WITH_MP3)
    expected += ",mp3";
#endif
#if defined(SQUEEZE2RAOP2_WITH_AAC)
    expected += ",aac";
#endif
#if defined(SQUEEZE2RAOP2_WITH_OGG)
    expected += ",ogg";
#endif
#if defined(SQUEEZE2RAOP2_WITH_OPUS)
    expected += ",ops";
#endif

    std::string actual;
    for (const CodecInfo& codec : supportedCodecs()) {
        expect(codec.capToken != nullptr && codec.capToken[0] != '\0',
               "every codec has a caps token");
        expect(supportsFormat(codec.format), "table and predicate agree");
        const auto dec = Decoder::create(codec.format, PcmFormat{44100, 16, 2, false});
        expect(dec != nullptr, "every supported format has a factory");
        if (!actual.empty()) actual += ",";
        actual += codec.capToken;
    }
    expect(actual == expected, "codec cap tokens and advertised order");

    // A format LMS can name but this bridge never decodes: no factory, not
    // advertised, so the strm guard rejects it.
    expect(!supportsFormat(StreamFormat::Flac), "flac not advertised");
    expect(Decoder::create(StreamFormat::Flac, PcmFormat{44100, 16, 2, false}) == nullptr,
           "flac has no factory");

#if defined(SQUEEZE2RAOP2_WITH_MP3)
    expect(supportsFormat(StreamFormat::Mp3), "mp3 built in");
#else
    expect(!supportsFormat(StreamFormat::Mp3), "mp3 not built in");
#endif
#if defined(SQUEEZE2RAOP2_WITH_AAC)
    expect(supportsFormat(StreamFormat::Aac), "aac built in");
#else
    expect(!supportsFormat(StreamFormat::Aac), "aac not built in");
#endif
#if defined(SQUEEZE2RAOP2_WITH_OGG)
    expect(supportsFormat(StreamFormat::Ogg), "ogg built in");
#else
    expect(!supportsFormat(StreamFormat::Ogg), "ogg not built in");
#endif
#if defined(SQUEEZE2RAOP2_WITH_OPUS)
    expect(supportsFormat(StreamFormat::Opus), "opus built in");
#else
    expect(!supportsFormat(StreamFormat::Opus), "opus not built in");
#endif
}