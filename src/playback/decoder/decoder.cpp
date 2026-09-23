#include "playback/decoder/decoder.h"

#include "common/log.h"
#include "playback/decoder/mp3_decoder.h"
#include "playback/decoder/pcm_decoder.h"
#if defined(SQUEEZE2RAOP2_WITH_AAC)
#include "playback/decoder/aac_decoder.h"
#endif

#include <algorithm>
#include <cmath>

namespace squeeze2raop2 {

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
    const double sec = double(windowMs) / 1000.0;
    if (sec < 5.0) return pcmAppliedRate_;

    const uint64_t prevReceived = pcmWindowReceivedBytes_;
    pcmWindowReceivedBytes_ = receivedBytes;
    if (prevReceived == 0 || receivedBytes < prevReceived) return pcmAppliedRate_;

    constexpr double kNominal = 44100.0;
    const double fps = double(receivedBytes - prevReceived) / double(inputFrameBytes_) / sec;
    if (fps < 0.95 * kNominal || fps > 1.05 * kNominal) return pcmAppliedRate_;  // stall/burst

    const double off = std::abs(fps - kNominal);
    // Gentle rebuild while below the prebuffer reserve.
    const double overdrive = queued < 131072 ? 1.002 : 1.0;
    if (pcmAppliedRate_ == 0.0) {
        if (off > 44.0) {  // 0.1%
            pcmAppliedRate_ = fps / overdrive;
            setSourceRate(pcmAppliedRate_);
            log::warn(log::Area::Dec, "pcm source {} fps ({} ppm off): regulating", fps,
                      int((fps - kNominal) / kNominal * 1e6));
        }
        return pcmAppliedRate_;
    }
    if (off <= 20.0) {  // back within ~0.045%: release to pass-through
        pcmAppliedRate_ = 0.0;
        setSourceRate(kNominal);
        log::info(log::Area::Dec, "pcm source rate nominal: pass-through");
        return pcmAppliedRate_;
    }
    // Keep regulating; refresh the overdrive decision.
    const double target = fps / overdrive;
    if (std::abs(target - pcmAppliedRate_) > 2.0) {
        pcmAppliedRate_ = target;
        setSourceRate(target);
    }
    return pcmAppliedRate_;
}

std::unique_ptr<Decoder> Decoder::create(StreamFormat format, const PcmFormat& in,
                                         uint32_t outputRate, uint8_t containerCode) {
    switch (format) {
    case StreamFormat::Mp3: return std::make_unique<Mp3Decoder>(in);
    case StreamFormat::Pcm: return std::make_unique<PcmDecoder>(in, outputRate);
#if defined(SQUEEZE2RAOP2_WITH_AAC)
    case StreamFormat::Aac: return std::make_unique<AacDecoder>(in, containerCode);
#endif
    default: return nullptr;
    }
}

}  // namespace squeeze2raop2
