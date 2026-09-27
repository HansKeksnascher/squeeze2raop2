#include "playback/decoder/decoder.h"

#include "common/log.h"
#include "common/util.h"
#include "playback/decoder/pcm_decoder.h"
#if defined(SQUEEZE2RAOP2_WITH_MP3)
#include "playback/decoder/mp3_decoder.h"
#endif
#if defined(SQUEEZE2RAOP2_WITH_AAC)
#include "playback/decoder/aac_decoder.h"
#endif
#if defined(SQUEEZE2RAOP2_WITH_OGG)
#include "playback/decoder/ogg_decoder.h"
#endif
#if defined(SQUEEZE2RAOP2_WITH_OPUS)
#include "playback/decoder/opus_decoder.h"
#endif

#include <algorithm>
#include <cmath>

namespace squeeze2raop2 {

namespace {

// Thin factory shims so the codec table can hold plain function pointers
// without naming every decoder type in the public header.
std::unique_ptr<Decoder> makePcm(const PcmFormat& in, uint8_t) {
    return std::make_unique<PcmDecoder>(in);
}
#if defined(SQUEEZE2RAOP2_WITH_MP3)
std::unique_ptr<Decoder> makeMp3(const PcmFormat& in, uint8_t) {
    return std::make_unique<Mp3Decoder>(in);
}
#endif
#if defined(SQUEEZE2RAOP2_WITH_AAC)
std::unique_ptr<Decoder> makeAac(const PcmFormat& in, uint8_t containerCode) {
    return std::make_unique<AacDecoder>(in, containerCode);
}
#endif
#if defined(SQUEEZE2RAOP2_WITH_OGG)
std::unique_ptr<Decoder> makeOgg(const PcmFormat& in, uint8_t) {
    return std::make_unique<OggDecoder>(in);
}
#endif
#if defined(SQUEEZE2RAOP2_WITH_OPUS)
std::unique_ptr<Decoder> makeOpus(const PcmFormat& in, uint8_t) {
    return std::make_unique<OggOpusDecoder>(in);
}
#endif

}  // namespace

Decoder::Decoder(const PcmFormat& input) : input_(input) {
    inputFrameBytes_ = size_t(input.channels) * (input.bitsPerSample / 8);
    if (!inputFrameBytes_) inputFrameBytes_ = 4;
}

PcmFormat Decoder::format() const {
    const PcmFormat decoded = decodedFormat();
    return decoded.sampleRate != 0 ? decoded : input_;
}

std::span<const int16_t> Decoder::nextChunk() {
    const size_t n = drain(chunk_);
    if (!n) return {};
    return std::span<const int16_t>(chunk_).first(n);
}

size_t Decoder::takeSamples(std::span<int16_t> out, std::vector<int16_t>& pcm) {
    const size_t n = std::min(pcm.size(), out.size());
    if (!n) return 0;
    std::ranges::copy(std::span{pcm}.first(n), out.begin());
    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
}

void Decoder::compactConsumed(std::vector<std::byte>& buffer, size_t& consumed) {
    if (consumed < kCompactThreshold) return;
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
    consumed = 0;
}

std::span<const CodecInfo> supportedCodecs() {
    // The one place codec enablement is decided. Order is the advertised HELO
    // caps order (pcm, mp3, aac, ogg, ops); keep it stable.
    static constexpr CodecInfo kCodecs[] = {
        {StreamFormat::Pcm, kCodecCapPcm, &makePcm},
#if defined(SQUEEZE2RAOP2_WITH_MP3)
        {StreamFormat::Mp3, kCodecCapMp3, &makeMp3},
#endif
#if defined(SQUEEZE2RAOP2_WITH_AAC)
        {StreamFormat::Aac, kCodecCapAac, &makeAac},
#endif
#if defined(SQUEEZE2RAOP2_WITH_OGG)
        {StreamFormat::Ogg, kCodecCapOgg, &makeOgg},
#endif
#if defined(SQUEEZE2RAOP2_WITH_OPUS)
        {StreamFormat::Opus, kCodecCapOpus, &makeOpus},
#endif
    };
    return std::span<const CodecInfo>(kCodecs);
}

bool supportsFormat(StreamFormat format) {
    return std::ranges::any_of(supportedCodecs(),
                               [format](const CodecInfo& codec) { return codec.format == format; });
}

std::unique_ptr<Decoder> Decoder::create(StreamFormat format, const PcmFormat& in,
                                         uint8_t containerCode) {
    for (const CodecInfo& codec : supportedCodecs())
        if (codec.format == format) return codec.create(in, containerCode);
    return nullptr;
}

}  // namespace squeeze2raop2
