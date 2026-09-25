// Stepper rules for the receiver-initiated volume chase: the receiver reports
// an absolute target while LMS only moves in BUTN button steps, so each step
// must know when to press, when to stop on convergence, and when a step has
// already overshot.

#include "playback/volume_map.h"

#include "check.h"

using namespace squeeze2raop2;
using namespace squeeze2raop2::test;

SQ2_TEST(remote_vol, direction) {
    expect(decideVolumeNudge(60.0, 50.0, 0, 10, 1.0) == VolumeNudge::Up,
           "target above -> volume up");
    expect(decideVolumeNudge(40.0, 50.0, 0, 10, 1.0) == VolumeNudge::Down,
           "target below -> volume down");
}

SQ2_TEST(remote_vol, convergence) {
    expect(decideVolumeNudge(50.0, 50.0, 0, 10, 1.0) == VolumeNudge::Reached, "exact target stops");
    expect(decideVolumeNudge(50.0, 50.5, 0, 10, 1.0) == VolumeNudge::Reached,
           "within tolerance stops");
    expect(decideVolumeNudge(50.5, 50.0, 0, 10, 1.0) == VolumeNudge::Reached,
           "within tolerance stops (above)");
}

SQ2_TEST(remote_vol, overshoot_guard) {
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

SQ2_TEST(remote_vol, budget) {
    expect(decideVolumeNudge(90.0, 50.0, 1, 0, 1.0) == VolumeNudge::Exhausted,
           "no steps left stops");
    expect(decideVolumeNudge(90.0, 50.0, 0, 0, 1.0) == VolumeNudge::Exhausted,
           "budget checked before direction");
    expect(decideVolumeNudge(90.0, 50.0, 1, 1, 1.0) == VolumeNudge::Up, "one more step allowed");
}