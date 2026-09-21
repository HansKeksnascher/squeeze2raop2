#pragma once

// LMS volume -> AirPlay volume mapping, philippe44-style (squeeze2raop):
// a piecewise-linear dB anchor table applied to the LMS slider percent.
//
// Reference chain (LMS-Raop application/squeeze2raop.c): LMS pre-encodes the
// slider as an "old gain" efficiency value (Squeezebox2 volume_map, 0..128),
// which the bridge re-inverts with its LMSVolumeMap table before indexing a
// dB anchor table ("VolumeMapping" config string). The two tables are mutual
// inverses, so the net mapping is dB anchors on the slider percent - which is
// exactly what our AUDG 16.16-gain decode already recovers directly. We
// implement the anchor table (his default was "-30:1, -15:50, 0:100"); the
// knee pair makes the quiet end of the slider spend dB budget instead of
// dying into the -30 dB protocol floor.
//
// Wire domain: AirPlay pct 0..100 -> -30..0 dBFS, pct 0 = -144 mute sentinel,
// so 0 is reserved for true mute and non-mute levels floor at 0.05.

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace squeeze2raop2 {

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