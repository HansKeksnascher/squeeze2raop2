#include "playback/volume_map.h"

#include "common/util.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>

namespace squeeze2raop2 {

namespace {

// Slim::Player::SqueezePlay::getVolumeParameters, consumed by
// Slim::Player::Squeezebox2::getVolume. See lmsSliderPctFromGain below.
constexpr double kLmsTotalVolumeRange = -74.0;  // dB at slider 0
constexpr double kLmsStepPoint = 25.0;          // slider position of the knee
constexpr double kLmsStepFraction = 0.5;
constexpr double kLmsMaxVolumeDb = 0.0;  // maximumVolume (slider 100)

constexpr double kLmsStepDb = kLmsTotalVolumeRange * kLmsStepFraction;  // -37 dB
constexpr double kLmsSlopeHigh =
    (kLmsMaxVolumeDb - kLmsStepDb) / (100.0 - kLmsStepPoint);  // 37/75 dB/step
constexpr double kLmsSlopeLow =
    (kLmsStepDb - kLmsTotalVolumeRange) / (kLmsStepPoint - 0.0);  // 37/25 dB/step

}  // namespace

double dbFromAirplayPct(double pct) { return kAirplayFloorDb + kAirplayDbPerPct * pct; }

double lmsSliderPctFromGain(uint32_t newGain) {
    if (newGain == 0) return 0.0;  // LMS mute
    const double db = 20.0 * std::log10(static_cast<double>(newGain) / 65536.0);
    const double pct = (db >= kLmsStepDb) ? kLmsStepPoint + (db - kLmsStepDb) / kLmsSlopeHigh
                                          : (db - kLmsTotalVolumeRange) / kLmsSlopeLow;
    return std::clamp(pct, 0.0, 100.0);
}

std::optional<VolumeAnchors> VolumeAnchors::parse(std::string_view spec) {
    VolumeAnchors out;
    size_t pos = 0;
    bool any = false;
    while (pos <= spec.size()) {
        size_t comma = spec.find(',', pos);
        size_t end = (comma == std::string_view::npos) ? spec.size() : comma;
        std::string_view item = spec.substr(pos, end - pos);
        pos = end + 1;

        item = trimView(item);
        if (item.empty()) continue;

        size_t colon = item.find(':');
        if (colon == std::string_view::npos) return std::nullopt;
        double db = 0, pct = 0;
        auto [dbEnd, dbEc] = std::from_chars(item.data(), item.data() + colon, db);
        if (dbEc != std::errc{} || dbEnd != item.data() + colon) return std::nullopt;
        auto [pctEnd, pctEc] =
            std::from_chars(item.data() + colon + 1, item.data() + item.size(), pct);
        if (pctEc != std::errc{} || pctEnd != item.data() + item.size()) return std::nullopt;
        // pct 0 is the mute sentinel, kept out of the anchor table
        if (db > 0.0 || db < -144.0 || pct < 1.0 || pct > 100.0) return std::nullopt;
        out.points_.emplace_back(pct, db);
        any = true;
    }
    if (!any) return std::nullopt;
    std::sort(out.points_.begin(), out.points_.end());
    // duplicate slider positions make the interpolation ambiguous
    if (std::adjacent_find(out.points_.begin(), out.points_.end(),
                           [](const auto& a, const auto& b) { return a.first >= b.first; }) !=
        out.points_.end())
        return std::nullopt;
    return out;
}

double VolumeAnchors::dbAt(double pct) const {
    const auto& p = points_;
    if (p.empty()) return 0.0;
    if (pct <= p.front().first) return p.front().second;
    if (pct >= p.back().first) return p.back().second;
    for (size_t i = 1; i < p.size(); ++i) {
        if (pct <= p[i].first) {
            double p1 = p[i - 1].first, d1 = p[i - 1].second;
            double p2 = p[i].first, d2 = p[i].second;
            return d1 + (pct - p1) * (d2 - d1) / (p2 - p1);
        }
    }
    return p.back().second;
}

double VolumeAnchors::airplayPctFromLms(double lmsPct) const {
    if (lmsPct <= 0.0) return 0.0;  // LMS mute -> -144 mute sentinel
    double db = dbAt(std::clamp(lmsPct, 0.0, 100.0));
    // AirPlay pct for a dBFS level: pct = (db - floor) / slope. The floor
    // anchor lands at pct 0 = mute, so non-mute levels floor at the quietest
    // step.
    double pct = (db - kAirplayFloorDb) / kAirplayDbPerPct;
    return clampAirVolumePct(std::max(pct, 0.05));
}

double VolumeAnchors::lmsPctFromDb(double db) const {
    const auto& p = points_;
    if (p.empty()) return 0.0;
    if (db <= p.front().second) return 1.0;  // quiet-but-not-mute floor
    if (db >= p.back().second) return 100.0;
    for (size_t i = 1; i < p.size(); ++i) {
        if (db <= p[i].second) {
            const double p1 = p[i - 1].first, d1 = p[i - 1].second;
            const double p2 = p[i].first, d2 = p[i].second;
            if (d2 == d1) return p2;
            return p1 + (db - d1) * (p2 - p1) / (d2 - d1);
        }
    }
    return 100.0;
}

}  // namespace squeeze2raop2