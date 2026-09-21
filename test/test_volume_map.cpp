#include "volume_map.h"

#include "check.h"

#include <cmath>

using namespace sq2t;
using squeeze2raop2::VolumeAnchors;
using squeeze2raop2::kDefaultVolumeMap;

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

} // namespace

int main() {
    testParse();
    testChain();
    std::printf("ok\n");
    return 0;
}