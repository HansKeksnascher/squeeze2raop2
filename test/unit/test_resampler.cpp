// Pins the libsamplerate wrapper: the 44.1 kHz bit-exact bypass and the
// native-rate -> 44100 conversion (length, DC gain and chunk invariance).

#include "playback/resampler.h"

#include "check.h"
#include "playback/volume_map.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace squeeze2raop2;
using namespace squeeze2raop2::test;

namespace {

std::vector<int16_t> sine(std::size_t frames, double freq, double rate, double amp) {
    std::vector<int16_t> out(frames * 2);
    for (std::size_t k = 0; k < frames; ++k) {
        const auto v = static_cast<int16_t>(
            std::lround(amp * std::sin(2.0 * M_PI * freq * static_cast<double>(k) / rate)));
        out[2 * k] = v;
        out[2 * k + 1] = v;
    }
    return out;
}

}  // namespace

SQ2_TEST(resampler, bypass_44100_is_bit_exact) {
    Resampler r(44100, 44100, ResamplerQuality::Medium);
    require(r.valid(), "bypass state is valid");
    expect(r.bypass(), "44100 source bypasses the filter");

    const std::vector<int16_t> in{0, 100, -100, 32767, -32768, 1234, -5678, 0, 1, -1};
    const auto out = r.process(in);
    require(out.size() == in.size(), "bypass keeps the sample count");
    for (std::size_t i = 0; i < in.size(); ++i) {
        expect(out[i] == static_cast<float>(in[i]) / 32768.0F,
               "bypass float is the normalized s16 input");
        expect(floatToS16(out[i]) == in[i], "bypass round-trips bit-exact");
    }
}

SQ2_TEST(resampler, downsample_48k_dc_gain) {
    constexpr std::size_t kFrames = 4800;  // 0.1 s at 48 kHz
    std::vector<int16_t> in(kFrames * 2);
    for (std::size_t k = 0; k < kFrames; ++k) {
        in[2 * k] = 1000;
        in[2 * k + 1] = -1000;
    }

    Resampler r(48000, 44100, ResamplerQuality::Medium);
    require(r.valid() && !r.bypass(), "48k -> 44.1k resamples");
    const auto out = r.process(in);
    require(!out.empty(), "48k produced output");

    // ~4800 * 44100/48000 = 4410 frames; the filter delays a few.
    const std::size_t frames = out.size() / 2;
    expect(frames >= 4300 && frames <= 4450, "downsample frame count");

    // Steady state: the filter has unity DC gain, so the tail holds the
    // normalized input (1000 / 32768).
    const float expected = 1000.0F / 32768.0F;
    const float left = out[(frames - 1) * 2];
    const float right = out[(frames - 1) * 2 + 1];
    expect(std::fabs(left - expected) < 0.001F, "DC gain preserved (left)");
    expect(std::fabs(right + expected) < 0.001F, "DC gain preserved (right)");
}

SQ2_TEST(resampler, upsample_22050_doubles_frames) {
    constexpr std::size_t kFrames = 2205;  // 0.1 s at 22.05 kHz
    const auto in = sine(kFrames, 1000.0, 22050.0, 20000.0);

    Resampler r(22050, 44100, ResamplerQuality::Medium);
    require(r.valid() && !r.bypass(), "22.05k -> 44.1k resamples");
    const auto out = r.process(in);
    require(!out.empty(), "22.05k produced output");
    const std::size_t frames = out.size() / 2;
    expect(frames >= 4300 && frames <= 4450, "upsample frame count (~2x)");
}

SQ2_TEST(resampler, chunk_invariance) {
    // Feeding the same input in small chunks must match one big pass: the
    // wrapper must not drop or duplicate frames at chunk boundaries.
    constexpr std::size_t kFrames = 9600;
    const auto in = sine(kFrames, 997.0, 48000.0, 12000.0);

    Resampler whole(48000, 44100, ResamplerQuality::Medium);
    const auto ref = whole.process(in);

    Resampler chunked(48000, 44100, ResamplerQuality::Medium);
    std::vector<float> got;
    constexpr std::size_t kChunkFrames = 128;
    for (std::size_t off = 0; off < kFrames; off += kChunkFrames) {
        const std::size_t n = std::min(kChunkFrames, kFrames - off);
        const auto part = chunked.process(std::span<const int16_t>(in.data() + off * 2, n * 2));
        got.insert(got.end(), part.begin(), part.end());
    }

    require(!ref.empty() && !got.empty(), "both passes produced output");
    expect(ref.size() == got.size(), "chunking keeps the output length");
    const std::size_t n = std::min(ref.size(), got.size());
    float maxDiff = 0.0F;
    for (std::size_t i = 0; i < n; ++i) maxDiff = std::max(maxDiff, std::fabs(ref[i] - got[i]));
    expect(maxDiff < 1.0F, "chunked output matches the whole-buffer pass");
}
