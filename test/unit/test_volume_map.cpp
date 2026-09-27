#include "playback/volume_map.h"

#include "check.h"

#include <cmath>
#include <cstdint>

using namespace squeeze2raop2;
using namespace squeeze2raop2::test;

using squeeze2raop2::dbFromAirplayPct;
using squeeze2raop2::kAirplayDbPerPct;
using squeeze2raop2::kAirplayFloorDb;
using squeeze2raop2::kDefaultVolumeMap;
using squeeze2raop2::lmsSliderPctFromGain;
using squeeze2raop2::VolumeAnchors;

namespace {

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// 16.16 fixed-point multiplier for a linear amplitude of 10^(db/20).
uint32_t gainForDb(double db) {
    return static_cast<uint32_t>(std::lround(std::pow(10.0, db / 20.0) * 65536.0));
}

}  // namespace

SQ2_TEST(volume_map, parse) {
    auto v = VolumeAnchors::parse(kDefaultVolumeMap);
    expect(v.has_value(), "default spec parses");
    expect(v->points().size() == 4, "four anchors");
    expect(near(v->dbAt(1.0), -30.0, 1e-9), "anchor 1 -> -30 dB");
    expect(near(v->dbAt(16.0), -23.0, 1e-9), "anchor 16 -> -23 dB");
    expect(near(v->dbAt(50.0), -15.0, 1e-9), "anchor 50 -> -15 dB");
    expect(near(v->dbAt(100.0), 0.0, 1e-9), "anchor 100 -> 0 dB");
    // between anchors: linear in dB per slider step
    expect(near(v->dbAt(33.0), -19.0, 1e-6), "interpolation 16..50");
    expect(near(v->dbAt(0.4), -30.0, 1e-9), "below first anchor clamps");
    expect(near(v->dbAt(150.0), 0.0, 1e-9), "above last anchor clamps");

    expect(!VolumeAnchors::parse(""), "empty spec rejected");
    expect(!VolumeAnchors::parse("nonsense"), "garbage rejected");
    expect(!VolumeAnchors::parse("-30"), "missing pct rejected");
    expect(!VolumeAnchors::parse("-30:"), "empty pct rejected");
    expect(!VolumeAnchors::parse("-30:0"), "pct 0 reserved for mute");
    expect(!VolumeAnchors::parse("-30:101"), "pct > 100 rejected");
    expect(!VolumeAnchors::parse("5:50"), "positive dB rejected");
    expect(!VolumeAnchors::parse("-30:1, -23:1"), "duplicate pct rejected");
    expect(!VolumeAnchors::parse("-30:1, -23:16x"), "trailing junk rejected");
    // The dB axis must be strictly increasing too: lmsPctFromDb assumes it, so
    // a config that is louder at a lower slider step would invert wrongly.
    expect(!VolumeAnchors::parse("-30:1, -20:50, -25:100"), "non-monotonic dB rejected");
    expect(!VolumeAnchors::parse("-30:1, -30:50, 0:100"), "duplicate dB rejected");

    // unsorted input is accepted and sorted
    auto u = VolumeAnchors::parse("-15:50, -30:1, 0:100");
    expect(u.has_value(), "unsorted spec parses");
    expect(u->points().front().first == 1.0, "sorted by pct");
    expect(near(u->dbAt(16.0), -25.41, 0.01), "single-segment interpolation");
}

SQ2_TEST(volume_map, chain) {
    auto v = VolumeAnchors::parse(kDefaultVolumeMap);
    expect(v.has_value(), "chain spec parses");
    expect(v->airplayPctFromLms(0.0) == 0.0, "LMS mute -> pct 0");
    expect(v->airplayPctFromLms(-3.0) == 0.0, "negative pct -> mute");
    expect(near(v->airplayPctFromLms(0.4), 0.05, 1e-9), "tiny gain floors, never mutes");
    expect(near(v->airplayPctFromLms(1.0), 0.05, 1e-9), "-30 dB anchor floors, not mute");
    expect(near(v->airplayPctFromLms(16.0), 23.333, 0.01), "slider 16 -> -23 dB");
    expect(near(v->airplayPctFromLms(50.0), 50.0, 1e-6), "slider 50 -> -15 dB");
    expect(near(v->airplayPctFromLms(100.0), 100.0, 1e-6), "slider 100 -> 0 dB");

    // monotonic across the whole slider
    double prev = 0.0;
    for (int s = 1; s <= 100; ++s) {
        double p = v->airplayPctFromLms(s);
        expect(p >= prev - 1e-9, "mapping monotonic");
        prev = p;
    }
}

SQ2_TEST(volume_map, inverse) {
    auto v = VolumeAnchors::parse(kDefaultVolumeMap);
    expect(v.has_value(), "inverse spec parses");
    expect(near(v->lmsPctFromDb(-30.0), 1.0, 1e-9), "-30 dB -> quiet-but-not-mute floor");
    expect(near(v->lmsPctFromDb(-45.0), 1.0, 1e-9), "below the floor clamps to 1");
    expect(near(v->lmsPctFromDb(-23.0), 16.0, 1e-9), "anchor dB -> slider 16");
    expect(near(v->lmsPctFromDb(-15.0), 50.0, 1e-9), "anchor dB -> slider 50");
    expect(near(v->lmsPctFromDb(0.0), 100.0, 1e-9), "0 dB -> slider 100");
    expect(near(v->lmsPctFromDb(6.0), 100.0, 1e-9), "above the top clamps to 100");

    // dB -> slider -> dB round-trips every anchor exactly.
    for (int s = 1; s <= 100; ++s) {
        const double pct = v->lmsPctFromDb(v->dbAt(s));
        expect(near(pct, s, 1e-6), "inverse round-trips the anchor curve");
    }
}

SQ2_TEST(volume_map, airplay_db) {
    // The wire domain shared by the forward and inverse receiver-volume paths.
    expect(near(dbFromAirplayPct(0.0), -30.0, 1e-9), "pct 0 -> floor");
    expect(near(dbFromAirplayPct(100.0), 0.0, 1e-9), "pct 100 -> 0 dB");
    expect(near(dbFromAirplayPct(23.333), -23.0, 0.01), "pct 23.333 -> -23 dB");
    // The floor/slope pair is a shared contract, not a self-derivation: pin both
    // constants so the forward and inverse mappings cannot drift apart.
    expect(near(kAirplayFloorDb, -30.0, 1e-9), "floor is -30 dB");
    expect(near(kAirplayDbPerPct, 0.3, 1e-12), "slope is 0.3 dB per pct");
}

SQ2_TEST(volume_map, clamp_air_volume_pct) {
    // Exactly 0 keeps the -144 dB mute sentinel.
    expect(clampAirVolumePct(0.0) == 0.0, "zero stays mute");
    expect(clampAirVolumePct(-3.0) == 0.0, "negative clamps to mute");
    expect(clampAirVolumePct(0.001) == 0.05, "tiny gain floors, never mutes");
    expect(clampAirVolumePct(0.049) == 0.05, "below floor clamps up");
    expect(clampAirVolumePct(0.05) == 0.05, "at floor unchanged");
    expect(clampAirVolumePct(50.22) == 50.22, "mid-range unchanged");
    expect(clampAirVolumePct(100.0) == 100.0, "full scale unchanged");
    expect(clampAirVolumePct(123.0) == 100.0, "overshoot clamps to 100");
}

// --- 16.16 gain and fades (volume_map.h) -----------------------------------

SQ2_TEST(volume_map, fixed_to_gain) {
    expect(fixedToGain(kFixedOne) == 1.0F, "unity");
    expect(fixedToGain(0) == 0.0F, "zero");
    expect(fixedToGain(kFixedOne / 2) == 0.5F, "half");
    expect(fixedToGain(kFixedOne * 2) == 2.0F, "double");
}

SQ2_TEST(volume_map, float_to_s16) {
    constexpr float kScale = 32768.0F;
    expect(floatToS16(0.0F) == 0, "zero");
    expect(floatToS16(1000.0F / kScale) == 1000, "positive integer is identity");
    expect(floatToS16(-2000.0F / kScale) == -2000, "negative integer is identity");
    expect(floatToS16(1.0F) == 32767, "positive full scale saturates");
    expect(floatToS16(-1.0F) == -32768, "negative full scale");
    expect(floatToS16(2.0F) == 32767, "positive overflow saturates");
    expect(floatToS16(-2.0F) == -32768, "negative overflow saturates");
    expect(floatToS16(0.4F / kScale) == 0, "rounds toward zero");
    expect(floatToS16(0.6F / kScale) == 1, "rounds up");
    expect(floatToS16(-0.6F / kScale) == -1, "rounds down (negative)");
    // Every s16 value survives the normalized float round-trip bit-exactly:
    // this is what makes the resampler bypass + identity gain bit-perfect at
    // 44.1 kHz.
    bool exact = true;
    for (int v = -32768; v <= 32767; ++v)
        if (floatToS16(static_cast<float>(v) / kScale) != static_cast<int16_t>(v)) exact = false;
    expect(exact, "s16 -> normalized float -> s16 round-trip is exact");
}

SQ2_TEST(volume_map, fade_ramp) {
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

// --- Receiver-volume chase step (volume_map.h) -----------------------------
//
// Pure per-step rule consumed by VolumeController; the paced stepper shell is
// pinned in test_volume_controller.cpp.

SQ2_TEST(volume_map, nudge_direction) {
    expect(decideVolumeNudge(60.0, 50.0, 0, 10, 1.0) == VolumeNudge::Up,
           "target above -> volume up");
    expect(decideVolumeNudge(40.0, 50.0, 0, 10, 1.0) == VolumeNudge::Down,
           "target below -> volume down");
}

SQ2_TEST(volume_map, nudge_convergence) {
    expect(decideVolumeNudge(50.0, 50.0, 0, 10, 1.0) == VolumeNudge::Reached, "exact target stops");
    expect(decideVolumeNudge(50.0, 50.5, 0, 10, 1.0) == VolumeNudge::Reached,
           "within tolerance stops");
    expect(decideVolumeNudge(50.5, 50.0, 0, 10, 1.0) == VolumeNudge::Reached,
           "within tolerance stops (above)");
}

SQ2_TEST(volume_map, nudge_overshoot_guard) {
    // Last step was up, but LMS is now past a lower target: stop, don't reverse
    // and oscillate. The tolerance check uses the exact difference, so 2 pct
    // is outside a 1 pct window.
    expect(decideVolumeNudge(60.0, 62.0, 1, 10, 1.0) == VolumeNudge::Overshoot,
           "stepped past an upward target");
    expect(decideVolumeNudge(40.0, 38.0, -1, 10, 1.0) == VolumeNudge::Overshoot,
           "stepped past a downward target");
    // A reversal that is not an overshoot is allowed (lastDir reset to 0 by the
    // caller on a fresh target) and simply steps back.
    expect(decideVolumeNudge(30.0, 50.0, 0, 10, 1.0) == VolumeNudge::Down,
           "fresh target may reverse direction");
}

SQ2_TEST(volume_map, nudge_budget) {
    expect(decideVolumeNudge(90.0, 50.0, 1, 0, 1.0) == VolumeNudge::Exhausted,
           "no steps left stops");
    expect(decideVolumeNudge(90.0, 50.0, 0, 0, 1.0) == VolumeNudge::Exhausted,
           "budget checked before direction");
    expect(decideVolumeNudge(90.0, 50.0, 1, 1, 1.0) == VolumeNudge::Up, "one more step allowed");
}

SQ2_TEST(volume_map, lms_curve) {
    // LMS mute is an explicit zero gain.
    expect(lmsSliderPctFromGain(0) == 0.0, "zero gain -> mute");

    // SqueezePlay/Boom curve (Slim::Player::SqueezePlay::getVolumeParameters):
    // slider 0 = -74 dB, 25 = -37 dB, 100 = 0 dB, knee at 25.
    expect(near(lmsSliderPctFromGain(gainForDb(0.0)), 100.0, 1e-6), "0 dB -> 100%");
    expect(near(lmsSliderPctFromGain(gainForDb(-37.0)), 25.0, 0.1), "-37 dB -> 25%");
    expect(near(lmsSliderPctFromGain(gainForDb(-24.667)), 50.0, 0.1), "-24.667 dB -> 50%");

    // The regression this curve fixes: under the old Squeezebox2 single-ramp
    // inverse, slider 16 (-50.3 dB) decoded to <= 0 and muted the bottom
    // quarter of the slider.
    const double pct16 = lmsSliderPctFromGain(gainForDb(-50.32));
    expect(pct16 > 15.5 && pct16 < 16.5, "slider-16 gain is not muted");
    expect(lmsSliderPctFromGain(gainForDb(-74.0)) == 0.0, "-74 dB -> mute");

    // monotonic, and clamped at both ends
    double prev = 0.0;
    for (uint32_t gain = 1; gain <= 65536; gain += 97) {
        const double pct = lmsSliderPctFromGain(gain);
        expect(pct >= prev - 1e-9, "recovered slider percent is monotonic");
        prev = pct;
    }
    expect(lmsSliderPctFromGain(1) == 0.0, "sub-audible gain clamps to mute");
    expect(near(lmsSliderPctFromGain(65536), 100.0, 1e-9), "full scale clamps to 100");
}