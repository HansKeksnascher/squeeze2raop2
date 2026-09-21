#include "volume_map.h"

#include "util.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>

namespace squeeze2raop2 {

std::optional<VolumeAnchors> VolumeAnchors::parse(std::string_view spec) {
    VolumeAnchors out;
    size_t pos = 0;
    bool any = false;
    while (pos <= spec.size()) {
        size_t comma = spec.find(',', pos);
        size_t end = (comma == std::string_view::npos) ? spec.size() : comma;
        std::string_view item = spec.substr(pos, end - pos);
        pos = end + 1;

        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
            item.remove_prefix(1);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
            item.remove_suffix(1);
        if (item.empty()) continue;

        size_t colon = item.find(':');
        if (colon == std::string_view::npos) return std::nullopt;
        double db = 0, pct = 0;
        auto [dbEnd, dbEc] = std::from_chars(item.data(), item.data() + colon, db);
        if (dbEc != std::errc{} || dbEnd != item.data() + colon) return std::nullopt;
        auto [pctEnd, pctEc] = std::from_chars(item.data() + colon + 1,
                                               item.data() + item.size(), pct);
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
    if (lmsPct <= 0.0) return 0.0;   // LMS mute -> -144 mute sentinel
    double db = dbAt(std::clamp(lmsPct, 0.0, 100.0));
    // AirPlay pct for a dBFS level: pct = (db + 30) / 0.3. The -30 dB anchor
    // lands at pct 0 = mute, so non-mute levels floor at the quietest step.
    double pct = (db + 30.0) / 0.3;
    return clampAirVolumePct(std::max(pct, 0.05));
}

} // namespace squeeze2raop2