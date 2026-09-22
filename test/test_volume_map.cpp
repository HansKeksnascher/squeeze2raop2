#include "volume_map.h"

#include "check.h"

#include <cmath>

using namespace sq2t;
using squeeze2raop2::kDefaultVolumeMap;
using squeeze2raop2::lmsSliderPctFromGain;
using squeeze2raop2::VolumeAnchors;

namespace {

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void testParse() {
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

    // unsorted input is accepted and sorted
    auto u = VolumeAnchors::parse("-15:50, -30:1, 0:100");
    expect(u.has_value(), "unsorted spec parses");
    expect(u->points().front().first == 1.0, "sorted by pct");
    expect(near(u->dbAt(16.0), -25.41, 0.01), "single-segment interpolation");
}

void testChain() {
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

// 16.16 fixed-point multiplier for a linear amplitude of 10^(db/20).
uint32_t gainForDb(double db) {
    return static_cast<uint32_t>(std::lround(std::pow(10.0, db / 20.0) * 65536.0));
}

void testLmsCurve() {
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

}  // namespace

int main() {
    testParse();
    testChain();
    testLmsCurve();
    std::printf("ok\n");
    return 0;
}