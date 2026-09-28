#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Forward declaration so the third-party C header stays out of first-party
// headers (samplerate.h defines `typedef struct SRC_STATE_tag SRC_STATE`).
struct SRC_STATE_tag;

namespace squeeze2raop2 {

// Resampler quality presets, selected by the global `resampler-quality`
// setting and mapped onto libsamplerate converters.
enum class ResamplerQuality : std::uint8_t {
    Best,    // SRC_SINC_BEST_QUALITY
    Medium,  // SRC_SINC_MEDIUM_QUALITY (default)
    Fast,    // SRC_SINC_FASTEST
    Linear,  // SRC_LINEAR (cheapest; for very weak hardware)
};

// Converts the decoders' native-rate interleaved s16 stereo to the AirPlay
// output clock (44100 Hz), in float. The whole downstream chain — gain/fade,
// skip-ahead, the debug sink and the output ring — then runs in this single
// 44.1 kHz float domain; one clamp at the edge is the only int16 boundary.
//
// A source already at the target rate is a lossless s16->float pass-through
// (no filter, no latency), so the sender's bit-exact 44.1 kHz path is
// preserved.
//
// One instance per stream, used from the stream thread only.
class Resampler {
public:
    Resampler(std::uint32_t sourceRate, std::uint32_t targetRate, ResamplerQuality quality);
    ~Resampler();

    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    // Feed one decoder chunk (interleaved s16 stereo at sourceRate) and return
    // the interleaved float output at targetRate, valid until the next call.
    // The span is empty when the state could not be created.
    std::span<const float> process(std::span<const int16_t> in);

    // Flush the filter tail at end of stream (end_of_input). Returns empty once
    // the tail is fully drained.
    std::span<const float> finish();

    bool bypass() const { return bypass_; }
    bool valid() const { return bypass_ || state_ != nullptr; }

private:
    // Run one SRC_DATA pass over `inFrames` frames already staged in inF_.
    std::span<const float> run(std::size_t inFrames, bool endOfInput);

    std::uint32_t sourceRate_;
    std::uint32_t targetRate_;
    double ratio_ = 1.0;
    bool bypass_ = false;
    SRC_STATE_tag* state_ = nullptr;
    std::vector<float> inF_;   // s16 -> float staging (interleaved)
    std::vector<float> outF_;  // resampled output (interleaved)
};

}  // namespace squeeze2raop2
