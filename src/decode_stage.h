#pragma once

#include "decoder/decoder.h"
#include "slimproto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace squeeze2raop2 {

// The decode half of a stream, on one thread: raw/container bytes in,
// interleaved s16 (native order) out, plus the output-ring telemetry and the
// PCM source-rate regulation that ride on the same cadence. No locks: every
// method runs on the stream thread.
class DecodeStage {
public:
    DecodeStage(StreamFormat format, const PcmFormat& input, uint32_t outputRate);

    void feed(std::span<const std::byte> in);
    void finish();  // end of input (flushes the MP3 tail)
    // Next decoded chunk in native s16; empty when the decoder is drained.
    // The span is valid until the next nextChunk()/feed() call.
    std::span<const int16_t> nextChunk();

    // Decoder output format; falls back to the input format until the decoder
    // is valid (an MP3 has no rate before its first frame).
    PcmFormat format() const;
    size_t pendingBytes() const;
    bool hasError() const;
    std::string_view name() const;
    // True while the PCM source-rate stage is stretching/shrinking (the
    // emitted timeline then runs ahead of the source; the pacer stands down).
    bool regulating() const { return pcmAppliedRate_ != 0.0; }

    // One telemetry sample per stream iteration: ring occupancy (`queued`
    // samples), source byte progress and time. Runs the 10 s summary and the
    // PCM rate regulator; returns the applied source rate (0 = pass-through).
    double observeOutput(size_t queued, uint64_t receivedBytes, uint64_t nowMs);

private:
    void regulateSourceRate(uint64_t receivedBytes, size_t queued, uint64_t windowMs);

    std::unique_ptr<Decoder> decoder_;
    std::array<int16_t, 1152 * 2> chunk_{};
    PcmFormat input_;             // fallback format until the decoder is valid
    size_t inputFrameBytes_ = 4;  // raw input frame size (rate regulator)
    // 10 s ring-occupancy telemetry.
    size_t ringStatsMin_ = SIZE_MAX;
    size_t ringStatsMax_ = 0;
    uint64_t ringStatsMarkMs_ = 0;
    uint64_t ringStarvedMs_ = 0;
    // PCM source-rate regulation.
    uint64_t pcmWindowReceivedBytes_ = 0;
    double pcmAppliedRate_ = 0.0;
};

}  // namespace squeeze2raop2
