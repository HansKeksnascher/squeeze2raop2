// Pins the pump's pure signal math: 16.16 gain with saturation, the linear
// fade ramp, and the skip-ahead interval conversion.

#include "playback/gain.h"

#include "check.h"

#include <cstdint>

using namespace squeeze2raop2::test;
using squeeze2raop2::applyGain16;
using squeeze2raop2::fadeGain16;
using squeeze2raop2::kFixedOne;
using squeeze2raop2::outputUnderrun;
using squeeze2raop2::skipFramesFor;

SQ2_TEST(gain, apply_gain16) {
    expect(applyGain16(1000, kFixedOne) == 1000, "unity gain is identity");
    expect(applyGain16(-2000, kFixedOne) == -2000, "unity keeps sign");
    expect(applyGain16(1000, 0) == 0, "zero gain silences");
    expect(applyGain16(1000, kFixedOne / 2) == 500, "half gain halves");
    expect(applyGain16(1000, kFixedOne * 2) == 2000, "2x gain doubles");
    // Boost past full scale saturates rather than wrapping.
    expect(applyGain16(30000, kFixedOne * 2) == 32767, "positive overflow saturates");
    expect(applyGain16(-30000, kFixedOne * 2) == -32768, "negative overflow saturates");
    // Attenuation: -6.02 dB ~ 0.5.
    const int32_t half = 0x8000;
    expect(applyGain16(32767, half) == 16383, "attenuation truncates toward zero");
}

SQ2_TEST(gain, fade_ramp) {
    const uint32_t dur = 1000;
    expect(fadeGain16(0, dur, true) == 0, "fade-in starts silent");
    expect(fadeGain16(dur, dur, true) == kFixedOne, "fade-in ends at unity");
    expect(fadeGain16(dur / 2, dur, true) == kFixedOne / 2, "fade-in midpoint is half");
    expect(fadeGain16(0, dur, false) == kFixedOne, "fade-out starts at unity");
    expect(fadeGain16(dur, dur, false) == 0, "fade-out ends silent");
    expect(fadeGain16(dur / 2, dur, false) == kFixedOne / 2, "fade-out midpoint is half");
    expect(fadeGain16(0, 0, false) == kFixedOne, "zero duration pins to unity");
    expect(fadeGain16(5000, dur, true) == kFixedOne, "past-end clamps (up)");
    expect(fadeGain16(5000, dur, false) == 0, "past-end clamps (down)");
}

SQ2_TEST(gain, skip_frames) {
    expect(skipFramesFor(1000, 44100) == 44100, "1 s at 44.1 kHz");
    expect(skipFramesFor(500, 48000) == 24000, "0.5 s at 48 kHz");
    expect(skipFramesFor(0, 44100) == 0, "zero ms skips nothing");
    expect(skipFramesFor(1, 44100) == 44, "1 ms truncates frames");
}

SQ2_TEST(gain, output_underrun) {
    expect(outputUnderrun(true, 0), "running with an empty ring underruns");
    expect(!outputUnderrun(true, 100), "running with queued audio is fine");
    expect(!outputUnderrun(false, 0), "an idle output is not an underrun");
}