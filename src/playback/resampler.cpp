#include "playback/resampler.h"

#include "common/log.h"

#include <samplerate.h>

#include <cmath>

namespace squeeze2raop2 {

namespace {

// The pipeline is fixed stereo s16 (decoders normalize mono/24-bit to
// interleaved s16 stereo), so the resampler is created for two channels.
constexpr int kChannels = 2;
// Extra output frames per pass: covers libsamplerate's rounding and any small
// internal buffering so a single SRC_DATA pass consumes the whole input.
constexpr std::size_t kOutputMarginFrames = 64;

int converterFor(ResamplerQuality quality) {
    switch (quality) {
    case ResamplerQuality::Best: return SRC_SINC_BEST_QUALITY;
    case ResamplerQuality::Fast: return SRC_SINC_FASTEST;
    case ResamplerQuality::Linear: return SRC_LINEAR;
    case ResamplerQuality::Medium: break;
    }
    return SRC_SINC_MEDIUM_QUALITY;
}

}  // namespace

Resampler::Resampler(std::uint32_t sourceRate, std::uint32_t targetRate, ResamplerQuality quality)
    : sourceRate_(sourceRate ? sourceRate : targetRate), targetRate_(targetRate) {
    // A source already at the target rate (or an unknown target) is a lossless
    // s16->float pass-through: no filter, no latency, bit-exact on the way back.
    bypass_ = targetRate_ == 0 || sourceRate_ == targetRate_;
    if (bypass_) return;

    ratio_ = static_cast<double>(targetRate_) / static_cast<double>(sourceRate_);
    int err = 0;
    state_ = src_new(converterFor(quality), kChannels, &err);
    if (!state_)
        log::error(log::Area::Pb, "libsamplerate init failed: {}", src_strerror(err));
    else
        log::info(log::Area::Pb, "resampler {} Hz -> {} Hz (ratio {:.6f})", sourceRate_,
                  targetRate_, ratio_);
}

Resampler::~Resampler() {
    if (state_) src_delete(state_);
}

std::span<const float> Resampler::process(std::span<const int16_t> in) {
    if (in.empty()) return {};
    if (bypass_) {
        inF_.resize(in.size());
        src_short_to_float_array(in.data(), inF_.data(), static_cast<int>(in.size()));
        return std::span<const float>(inF_);
    }
    if (!state_) return {};

    const std::size_t inFrames = in.size() / kChannels;
    inF_.resize(inFrames * kChannels);
    src_short_to_float_array(in.data(), inF_.data(), static_cast<int>(inFrames * kChannels));
    return run(inFrames, /*endOfInput=*/false);
}

std::span<const float> Resampler::finish() {
    if (bypass_ || !state_) return {};
    return run(/*inFrames=*/0, /*endOfInput=*/true);
}

std::span<const float> Resampler::run(std::size_t inFrames, bool endOfInput) {
    if (inF_.empty()) inF_.resize(kChannels, 0.0F);  // src_process needs a valid data_in

    const std::size_t capFrames =
        static_cast<std::size_t>(std::ceil(static_cast<double>(inFrames) * ratio_)) +
        kOutputMarginFrames;
    if (outF_.size() < capFrames * kChannels) outF_.resize(capFrames * kChannels);

    std::size_t inUsed = 0;
    std::size_t outGen = 0;
    for (;;) {
        const std::size_t inLeft = inFrames - inUsed;
        const std::size_t outLeft = outF_.size() / kChannels - outGen;

        SRC_DATA d{};
        d.data_in = inF_.data() + inUsed * kChannels;
        d.data_out = outF_.data() + outGen * kChannels;
        d.input_frames = static_cast<long>(inLeft);
        d.output_frames = static_cast<long>(outLeft);
        d.src_ratio = ratio_;
        d.end_of_input = endOfInput ? 1 : 0;

        const int err = src_process(state_, &d);
        if (err) {
            log::error(log::Area::Pb, "libsamplerate process failed: {}", src_strerror(err));
            return {};
        }
        inUsed += static_cast<std::size_t>(d.input_frames_used);
        outGen += static_cast<std::size_t>(d.output_frames_gen);

        if (inUsed >= inFrames) break;
        if (d.input_frames_used == 0 && d.output_frames_gen == 0) break;  // no progress
        if (outLeft == 0)  // output filled first: grow and keep going
            outF_.resize(outF_.size() + kOutputMarginFrames * kChannels);
    }
    return std::span<const float>(outF_.data(), outGen * kChannels);
}

}  // namespace squeeze2raop2
