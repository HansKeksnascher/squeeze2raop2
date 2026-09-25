#include "playback/decoder/decoder.h"

#include "common/log.h"
#include "common/util.h"
#include "playback/decoder/mp3_decoder.h"
#include "playback/decoder/pcm_decoder.h"
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
std::unique_ptr<Decoder> makePcm(const PcmFormat& in, uint32_t outputRate, uint8_t) {
    return std::make_unique<PcmDecoder>(in, outputRate);
}
std::unique_ptr<Decoder> makeMp3(const PcmFormat& in, uint32_t, uint8_t) {
    return std::make_unique<Mp3Decoder>(in);
}
#if defined(SQUEEZE2RAOP2_WITH_AAC)
std::unique_ptr<Decoder> makeAac(const PcmFormat& in, uint32_t, uint8_t containerCode) {
    return std::make_unique<AacDecoder>(in, containerCode);
}
#endif
#if defined(SQUEEZE2RAOP2_WITH_OGG)
std::unique_ptr<Decoder> makeOgg(const PcmFormat& in, uint32_t, uint8_t) {
    return std::make_unique<OggDecoder>(in);
}
#endif
#if defined(SQUEEZE2RAOP2_WITH_OPUS)
std::unique_ptr<Decoder> makeOpus(const PcmFormat& in, uint32_t, uint8_t) {
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

// Measure the source's arrival rate over the telemetry window. The socket
// feed IS the arrival (the reader strips icy meta and hands every audio byte
// to the decoder), so Δ(receivedBytes) is the source rate — any backlog or
// emission correction feeds back on itself and spirals. Regulate the pcm
// decoder to the 44100 output clock. Hysteresis: engage beyond 0.1%
// deviation, release below 0.045%. While the ring is below the prebuffer
// reserve, a 0.2% overdrive gently rebuilds the reserve (an inaudible
// ~8 cent pitch offset).
double Decoder::regulateRate(uint64_t receivedBytes, size_t queued, uint64_t windowMs) {
    if (!regulatesRate() || !inputFrameBytes_) return pcmAppliedRate_;
    const double sec = double(windowMs) / kMsPerSecond;
    if (sec < kRegulationMinWindowSec) return pcmAppliedRate_;

    const uint64_t prevReceived = pcmWindowReceivedBytes_;
    pcmWindowReceivedBytes_ = receivedBytes;
    if (prevReceived == 0 || receivedBytes < prevReceived) return pcmAppliedRate_;

    constexpr double kNominal = kRegulationNominalRate;
    const double fps = double(receivedBytes - prevReceived) / double(inputFrameBytes_) / sec;
    if (fps < kRegulationSanityLo * kNominal || fps > kRegulationSanityHi * kNominal)
        return pcmAppliedRate_;  // stall/burst

    const double off = std::abs(fps - kNominal);
    // Gentle rebuild while below the prebuffer reserve.
    const double overdrive = queued < kRegulationPrebufferBytes ? kRegulationOverdrive : 1.0;
    if (pcmAppliedRate_ == 0.0) {
        if (off > kRegulationEngage) {  // 0.1%
            pcmAppliedRate_ = fps / overdrive;
            setSourceRate(pcmAppliedRate_);
            log::warn(log::Area::Dec, "pcm source {} fps ({} ppm off): regulating", fps,
                      int((fps - kNominal) / kNominal * kRegulationPpmScale));
        }
        return pcmAppliedRate_;
    }
    if (off <= kRegulationRelease) {  // back within ~0.045%: release to pass-through
        pcmAppliedRate_ = 0.0;
        setSourceRate(kNominal);
        log::info(log::Area::Dec, "pcm source rate nominal: pass-through");
        return pcmAppliedRate_;
    }
    // Keep regulating; refresh the overdrive decision.
    const double target = fps / overdrive;
    if (std::abs(target - pcmAppliedRate_) > kRegulationRefresh) {
        pcmAppliedRate_ = target;
        setSourceRate(target);
    }
    return pcmAppliedRate_;
}

std::span<const CodecInfo> supportedCodecs() {
    // The one place codec enablement is decided. Order is the advertised HELO
    // caps order (pcm, mp3, aac, ogg, ops); keep it stable.
    static constexpr CodecInfo kCodecs[] = {
        {StreamFormat::Pcm, kCodecCapPcm, &makePcm},    {StreamFormat::Mp3, kCodecCapMp3, &makeMp3},
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
                                         uint32_t outputRate, uint8_t containerCode) {
    for (const CodecInfo& codec : supportedCodecs())
        if (codec.format == format) return codec.create(in, outputRate, containerCode);
    return nullptr;
}

}  // namespace squeeze2raop2
