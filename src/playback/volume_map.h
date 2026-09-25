#pragma once

// Volume handling for the LMS -> AirPlay bridge, in two stages:
//
//   1. AUDG gain -> LMS slider percent (lmsSliderPctFromGain): LMS sends a
//      16.16 fixed-point linear amplitude multiplier; inverting it recovers
//      the slider position the user sees.
//   2. LMS slider percent -> AirPlay dB attenuation (VolumeAnchors): a
//      piecewise-linear dB anchor table chosen for the receiver.
//
// Stage-2 reference chain (LMS-Raop application/squeeze2raop.c): LMS
// pre-encodes the slider with its volume_map table before indexing a dB
// anchor table ("VolumeMapping" config string). The two tables are mutual
// inverses, so the net mapping is dB anchors on the slider percent. We
// implement the anchor table (his default was "-30:1, -15:50, 0:100"); the
// knee pair makes the quiet end of the slider spend dB budget instead of
// dying into the -30 dB protocol floor.
//
// Wire domain: AirPlay pct 0..100 -> -30..0 dBFS, pct 0 = -144 mute sentinel,
// so 0 is reserved for true mute and non-mute levels floor at 0.05.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace squeeze2raop2 {

// --- Stage 1: AUDG gain -> LMS slider percent -----------------------------
//
// LMS does NOT use one global slider curve: it picks the gain from the player
// class it instantiated for the HELO deviceid. This bridge advertises
// deviceid 12, which LMS maps to 'SqueezePlay' (Slim/Networking/Slimproto.pm),
// and Slim::Player::SqueezePlay overrides getVolumeParameters with the Boom
// curve:
//
//   totalVolumeRange = -74 dB, stepPoint = 25, stepFraction = 0.5
//
// so the slider is piecewise-linear in dB with a knee at 25:
//   0  .. 25  ->  -74 .. -37 dB  (1.48 dB/step)
//   25 .. 100 ->  -37 .. 0 dB    (0.49333 dB/step)
//
// Inverting Squeezebox2's own single ramp (-50 dB, 0.495 dB/step) instead
// mutes the bottom quarter of the slider: at slider 16 the real gain is
// -50.3 dB, which the wrong inverse reads as percent <= 0.
//
// newGain is the 16.16 fixed-point linear amplitude multiplier (1.0 = 65536);
// returns the slider percent 0..100 (0 = mute).
double lmsSliderPctFromGain(uint32_t newGain);

// --- Stage 2: LMS slider percent -> AirPlay dB attenuation ----------------

// Default anchor spec: LMS slider 1 = -30 dB (protocol floor), 16 = -23 dB
// (quiet-listening knee), 50 = -15 dB, 100 = 0 dB (full scale).
inline constexpr const char* kDefaultVolumeMap = "-30:1, -23:16, -15:50, 0:100";

// AirPlay wire domain: percent 0..100 maps to -30..0 dBFS at 0.3 dB per pct,
// with percent 0 reserved as the -144 dB mute sentinel. Shared by the forward
// (slider -> AirPlay pct) and inverse (receiver dB -> slider) mappings so the
// floor/slope cannot drift between them.
inline constexpr double kAirplayFloorDb = -30.0;
inline constexpr double kAirplayDbPerPct = 0.3;

// AirPlay volume domain: pct 0..100, with 0 as the -144 dB mute sentinel.
constexpr double kAirplayPctMin = 0.0;
constexpr double kAirplayPctMax = 100.0;
constexpr double kAirplayMutePct = 0.0;
constexpr double kAirplayMuteDb = -144.0;
// Quietest audible step (-29.985 dB): a tiny nonzero LMS gain quantizes to ~0
// pct and would silently mute instead of playing the protocol floor, so it
// clamps up to this.
constexpr double kAirplayMinAudiblePct = 0.05;

// LMS slider domain.
constexpr double kLmsSliderMin = 0.0;
constexpr double kLmsSliderMax = 100.0;

// Clamp a linear volume percent to the AirPlay domain, preserving the exact
// mute sentinel and flooring tiny audible levels.
inline double clampAirVolumePct(double pct) {
    if (pct <= kAirplayMutePct) return kAirplayMutePct;
    return std::clamp(pct, kAirplayMinAudiblePct, kAirplayPctMax);
}

// AirPlay attenuation in dB for an AirPlay percent (0..100).
double dbFromAirplayPct(double pct);

class VolumeAnchors {
public:
    // Parses "db:pct, db:pct, ..." with ascending pct in 1..100 and
    // db <= 0. Levels below the first anchor clamp to its dB, above the
    // last to its dB; between anchors the interpolation is linear in dB
    // per slider step (philippe44 SetVolumeMapping semantics, with his
    // implicit mapping[0] = -144 handled by the mute rule instead).
    [[nodiscard]] static std::optional<VolumeAnchors> parse(std::string_view spec);

    // AirPlay attenuation in dB for a slider percent (0..100).
    double dbAt(double pct) const;

    // Full chain: LMS slider pct -> AirPlay pct for RaopSender::setVolume.
    // pct <= 0 is the mute sentinel (returns 0); a non-mute slider never
    // produces 0 (the -30 dB anchor would), it floors at 0.05 pct.
    double airplayPctFromLms(double lmsPct) const;

    // Inverse anchors: AirPlay attenuation in dB -> LMS slider percent.
    // Below the quietest anchor there is no representable quiet-but-not-mute
    // slider position, so it clamps to 1 (the caller maps a true mute sentinel
    // to 0); above the loudest anchor it clamps to 100. Used by the
    // receiver-initiated volume path to recover the LMS slider a receiver's
    // reported volume corresponds to.
    [[nodiscard]] double lmsPctFromDb(double db) const;

    const std::vector<std::pair<double, double>>& points() const { return points_; }

private:
    // (slider pct, dB), sorted ascending by pct.
    std::vector<std::pair<double, double>> points_;
};

// --- 16.16 fixed-point gain and fades -------------------------------------
//
// The pump's replay-gain and fade math (squeezelite parity). Header-only so the
// unit suite can pin the math without pulling in the pipeline.

inline constexpr int32_t kFixedOne = 0x10000;  // 1.0 in 16.16
constexpr int kFixedShift = 16;                // fractional bits in 16.16

// squeezelite's gain(): (gain * sample) >> 16. Applied to an s16 sample the
// result is the scaled s16 (gain 0x10000 is unity); saturate instead of
// wrapping if replay gain boosts past full scale.
inline int16_t applyGain16(int16_t sample, int32_t gain) {
    const int64_t res = (static_cast<int64_t>(gain) * static_cast<int64_t>(sample)) >> kFixedShift;
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

// --- Receiver-volume chase step -------------------------------------------
//
// An AirPlay 2 receiver (HomePod/Sonos) reports the volume its buttons set as
// an absolute unit volume; LMS only exposes relative volume buttons over
// slimproto (BUTN volup/voldown), so the bridge paces LMS toward the reported
// target one step at a time. The presses must be spaced so LMS treats each as
// a fresh press (not a held-button repeat, whose increment computes to zero).
// This is the pure decision for one step; VolumeController owns the pacing.

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