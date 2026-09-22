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

    const std::vector<std::pair<double, double>>& points() const { return points_; }

private:
    // (slider pct, dB), sorted ascending by pct.
    std::vector<std::pair<double, double>> points_;
};

}  // namespace squeeze2raop2