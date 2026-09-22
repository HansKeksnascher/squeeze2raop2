#include "decode_stage.h"

#include "log.h"

#include <algorithm>
#include <cmath>

namespace squeeze2raop2 {

DecodeStage::DecodeStage(StreamFormat format, const PcmFormat& input, uint32_t outputRate)
    : decoder_(Decoder::create(format, input, outputRate)), input_(input) {
    inputFrameBytes_ = size_t(input.channels) * (input.bitsPerSample / 8);
    if (!inputFrameBytes_) inputFrameBytes_ = 4;
}

void DecodeStage::feed(std::span<const std::byte> in) {
    if (decoder_) decoder_->feed(in);
}

void DecodeStage::finish() {
    if (decoder_) decoder_->finish();
}

std::span<const int16_t> DecodeStage::nextChunk() {
    if (!decoder_) return {};
    const size_t n = decoder_->drain(chunk_);
    if (!n) return {};
    return std::span<const int16_t>(chunk_).first(n);
}

PcmFormat DecodeStage::format() const {
    if (decoder_) {
        const PcmFormat decoded = decoder_->format();
        if (decoded.sampleRate != 0) return decoded;
    }
    return input_;
}

size_t DecodeStage::pendingBytes() const { return decoder_ ? decoder_->pendingBytes() : 0; }

bool DecodeStage::hasError() const { return decoder_ && decoder_->hasError(); }

std::string_view DecodeStage::name() const { return decoder_ ? decoder_->name() : "?"; }

double DecodeStage::observeOutput(size_t queued, uint64_t receivedBytes, uint64_t nowMs) {
    if (queued < ringStatsMin_) ringStatsMin_ = queued;
    if (queued > ringStatsMax_) ringStatsMax_ = queued;

    if (ringStatsMarkMs_ == 0) {
        ringStatsMarkMs_ = nowMs;
        pcmWindowReceivedBytes_ = 0;
    } else if (nowMs - ringStatsMarkMs_ >= 10000) {
        log::info("[ap] ring 10s: cur={} min={} max={} samples", queued, ringStatsMin_,
                  ringStatsMax_);
        regulateSourceRate(receivedBytes, queued, nowMs - ringStatsMarkMs_);
        ringStatsMin_ = SIZE_MAX;
        ringStatsMax_ = 0;
        ringStatsMarkMs_ = nowMs;
    }

    if (queued == 0) {
        if (!ringStarvedMs_) {
            ringStarvedMs_ = nowMs;
            log::warn("[ap] ring starved (0 samples)");
        }
    } else if (ringStarvedMs_) {
        log::info("[ap] ring recovered after {} ms", nowMs - ringStarvedMs_);
        ringStarvedMs_ = 0;
    }
    return pcmAppliedRate_;
}

// Measure the source's arrival rate over the telemetry window. The socket
// feed IS the arrival (the reader strips icy meta and hands every audio byte
// to the decoder), so Δ(receivedBytes) is the source rate — any backlog or
// emission correction feeds back on itself and spirals. Regulate the pcm
// decoder to the 44100 output clock. Hysteresis: engage beyond 0.1%
// deviation, release below 0.045%. While the ring is below the prebuffer
// reserve, a 0.2% overdrive gently rebuilds the reserve (an inaudible
// ~8 cent pitch offset).
void DecodeStage::regulateSourceRate(uint64_t receivedBytes, size_t queued, uint64_t windowMs) {
    if (!decoder_ || decoder_->name() != "pcm" || !inputFrameBytes_) return;
    const double sec = double(windowMs) / 1000.0;
    if (sec < 5.0) return;

    const uint64_t prevReceived = pcmWindowReceivedBytes_;
    pcmWindowReceivedBytes_ = receivedBytes;
    if (prevReceived == 0 || receivedBytes < prevReceived) return;

    constexpr double kNominal = 44100.0;
    const double fps = double(receivedBytes - prevReceived) / double(inputFrameBytes_) / sec;
    if (fps < 0.95 * kNominal || fps > 1.05 * kNominal) return;  // stall/burst

    const double off = std::abs(fps - kNominal);
    if (pcmAppliedRate_ == 0.0) {
        if (off > 44.0) {  // 0.1%
            // Gentle rebuild while below the prebuffer reserve.
            const double overdrive = queued < 131072 ? 1.002 : 1.0;
            pcmAppliedRate_ = fps / overdrive;
            decoder_->setSourceRate(pcmAppliedRate_);
            log::warn("[ap] pcm source {} fps ({} ppm off): regulating", fps,
                      int((fps - kNominal) / kNominal * 1e6));
        }
        return;
    }
    if (off <= 20.0) {  // back within ~0.045%: release to pass-through
        pcmAppliedRate_ = 0.0;
        decoder_->setSourceRate(kNominal);
        log::info("[ap] pcm source rate nominal: pass-through");
        return;
    }
    // Keep regulating; refresh the overdrive decision.
    const double overdrive = queued < 131072 ? 1.002 : 1.0;
    const double target = fps / overdrive;
    if (std::abs(target - pcmAppliedRate_) > 2.0) {
        pcmAppliedRate_ = target;
        decoder_->setSourceRate(target);
    }
}

}  // namespace squeeze2raop2
