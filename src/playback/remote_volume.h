#pragma once

// Receiver-initiated volume chase. An AirPlay 2 receiver (HomePod/Sonos)
// reports the volume its buttons set as an absolute unit volume; LMS only
// exposes relative volume buttons over slimproto (BUTN volup/voldown), so the
// bridge walks LMS toward the reported target one step at a time. The presses
// must be paced so LMS treats each as a fresh press (not a held-button
// repeat, whose increment computes to zero). This is the pure decision for one
// step, kept header-only so the overshoot/budget rules are unit-testable
// without a live LMS or receiver.

#include <cmath>

namespace squeeze2raop2 {

enum class VolumeNudge {
    Up,         // press volume up
    Down,       // press volume down
    Reached,    // within tolerance of the target: stop
    Overshoot,  // a step already carried LMS past the target: stop
    Exhausted,  // per-change step budget spent: stop
};

// `lastDir` is the direction of the previous step (-1 down, +1 up, 0 unset);
// `budget` is the number of steps still allowed for this target.
[[nodiscard]] inline VolumeNudge decideVolumeNudge(double targetPct, double lmsPct, int lastDir,
                                                   int budget, double tolerance) {
    if (std::fabs(targetPct - lmsPct) <= tolerance) return VolumeNudge::Reached;
    const int need = (targetPct - lmsPct) > 0.0 ? 1 : -1;
    if (lastDir != 0 && need != lastDir) return VolumeNudge::Overshoot;
    if (budget <= 0) return VolumeNudge::Exhausted;
    return need > 0 ? VolumeNudge::Up : VolumeNudge::Down;
}

}  // namespace squeeze2raop2