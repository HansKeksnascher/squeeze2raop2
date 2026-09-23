#pragma once

// Small, pure signal helpers for the pump: 16.16 fixed-point gain (replay
// gain and fades) and the skip-ahead interval conversion. Header-only so the
// unit suite can pin the math without pulling in the pipeline.

#include <algorithm>
#include <cstdint>

namespace squeeze2raop2 {

inline constexpr int32_t kFixedOne = 0x10000;  // 1.0 in 16.16

// squeezelite's gain(): (gain * sample) >> 16. Applied to an s16 sample the
// result is the scaled s16 (gain 0x10000 is unity); saturate instead of
// wrapping if replay gain boosts past full scale.
inline int16_t applyGain16(int16_t sample, int32_t gain) {
    const int64_t res = (static_cast<int64_t>(gain) * static_cast<int64_t>(sample)) >> 16;
    return static_cast<int16_t>(std::clamp<int64_t>(res, -32768, 32767));
}

// Linear amplitude ramp at frame `pos` of `dur`, in 16.16. up = 0->1,
// otherwise 1->0. dur == 0 or pos >= dur pins to the end value.
inline int32_t fadeGain16(uint32_t pos, uint32_t dur, bool up) {
    if (dur == 0) return kFixedOne;
    if (pos >= dur) return up ? kFixedOne : 0;
    const int32_t g = static_cast<int32_t>((static_cast<int64_t>(pos) * kFixedOne) / dur);
    return up ? g : (kFixedOne - g);
}

// Skip-ahead interval in ms -> source frames at `rate`. 0 ms -> 0 frames.
inline uint64_t skipFramesFor(uint32_t ms, uint32_t rate) {
    return (static_cast<uint64_t>(ms) * rate) / 1000u;
}

// STMo decision: the receiver output is running but its ring is empty while
// the HTTP source is still active (a network underrun, squeezelite parity).
inline bool outputUnderrun(bool running, size_t queued) { return running && queued == 0; }

}  // namespace squeeze2raop2