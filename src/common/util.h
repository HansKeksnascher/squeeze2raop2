#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace squeeze2raop2 {

uint32_t hash32(std::string_view s);

// Trim ASCII whitespace from both ends of `s` (view, no allocation).
std::string_view trimView(std::string_view s);

// ASCII-lowercased copy of `s`.
std::string toLower(std::string_view s);

// Parses mDNS TXT wire format: a sequence of (len byte, len-1 data bytes),
// each data chunk split at its first '=' into key/value. First occurrence
// of a key wins.
std::map<std::string, std::string> parseTxtKeyValues(std::string_view raw);

std::string macToString(const std::array<uint8_t, 6>& mac);
bool macFromString(std::string_view s, std::array<uint8_t, 6>& out);

std::array<uint8_t, 6> fakeMacFor(std::string_view deviceId);

// AirPlay volume domain: pct 0..100 maps to -30..0 dBFS with pct 0 as the
// -144 dB mute sentinel. Exactly 0 keeps the sentinel; a tiny nonzero LMS
// gain quantizes to ~0 pct and would silently mute instead of playing the
// protocol floor, so it clamps up to 0.05 pct (-29.985 dB, the quietest
// audible step).
inline double clampAirVolumePct(double pct) {
    if (pct <= 0.0) return 0.0;
    return std::clamp(pct, 0.05, 100.0);
}

uint64_t nowMs();

std::string urlDecode(std::string_view s);

// Thread-safe errno -> text (strerror_r/std::generic_category under the hood).
std::string errnoMessage(int err);

}  // namespace squeeze2raop2
